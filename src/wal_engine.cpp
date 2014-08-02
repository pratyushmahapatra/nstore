// WAL LOGGING

#include "wal_engine.h"
#include <fstream>

using namespace std;

void wal_engine::group_commit() {

  while (ready) {
    //std::cout << "Syncing log !" << endl;

    // sync
    fs_log.sync();

    std::this_thread::sleep_for(std::chrono::milliseconds(conf.gc_interval));
  }
}

wal_engine::wal_engine(const config& _conf, bool _read_only)
    : conf(_conf),
      db(conf.db) {

  etype = engine_type::WAL;
  read_only = _read_only;
  fs_log.configure(conf.fs_path + "log");

  vector<table*> tables = db->tables->get_data();
  for (table* tab : tables) {
    std::string table_file_name = conf.fs_path + std::string(tab->table_name);
    tab->fs_data.configure(table_file_name, tab->max_tuple_size, false);
  }

  // Logger start
  if (!read_only) {
    gc = std::thread(&wal_engine::group_commit, this);
    ready = true;
  }
}

wal_engine::~wal_engine() {

  // Logger end
  if (!read_only) {
    ready = false;
    gc.join();

    fs_log.sync();
    fs_log.close();

    vector<table*> tables = db->tables->get_data();
    for (table* tab : tables) {
      tab->fs_data.sync();
      tab->fs_data.close();
    }
  }
}

std::string wal_engine::select(const statement& st) {
  LOG_INFO("Select");
  std::string val;

  record* rec_ptr = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  table_index* table_index = tab->indices->at(st.table_index_id);
  std::string key_str = get_data(rec_ptr, table_index->sptr);

  LOG_INFO("val : --%s-- ", key_str.c_str());
  unsigned long key = hash_fn(key_str);
  off_t storage_offset;

  if (table_index->off_map->exists(key) == 0) {
    delete rec_ptr;
    return val;
  }

  storage_offset = table_index->off_map->at(key);
  val = tab->fs_data.at(storage_offset);
  val = deserialize_to_string(val, st.projection, false);

  LOG_INFO("val : %s", val.c_str());

  delete rec_ptr;
  return val;
}

int wal_engine::insert(const statement& st) {
  LOG_INFO("Insert");
  record* after_rec = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  plist<table_index*>* indices = tab->indices;

  unsigned int num_indices = tab->num_indices;
  unsigned int index_itr;

  std::string key_str = get_data(after_rec, indices->at(0)->sptr);
  LOG_INFO("key_str :: %s", key_str.c_str());
  unsigned long key = hash_fn(key_str);

  // Check if key exists
  if (indices->at(0)->off_map->exists(key) != 0) {
    delete after_rec;
    return EXIT_SUCCESS;
  }

  // Add log entry
  std::string after_tuple = serialize(after_rec, after_rec->sptr, false);
  entry_stream.str("");
  entry_stream << st.transaction_id << " " << st.op_type << " " << st.table_id
               << " " << after_tuple << "\n";
  entry_str = entry_stream.str();
  fs_log.push_back(entry_str);
  off_t storage_offset;

  storage_offset = tab->fs_data.push_back(after_tuple);
  LOG_INFO("Insert str :: --%s-- ", after_tuple.c_str());
  LOG_INFO("Insert offset :: %lu ", storage_offset);

  // Add entry in indices
  for (index_itr = 0; index_itr < num_indices; index_itr++) {
    key_str = get_data(after_rec, indices->at(index_itr)->sptr);
    key = hash_fn(key_str);

    indices->at(index_itr)->off_map->insert(key, storage_offset);
  }

  delete after_rec;
  return EXIT_SUCCESS;
}

int wal_engine::remove(const statement& st) {
  LOG_INFO("Remove");
  record* rec_ptr = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  plist<table_index*>* indices = tab->indices;

  unsigned int num_indices = tab->num_indices;
  unsigned int index_itr;
  record* before_rec = NULL;

  std::string key_str = get_data(rec_ptr, indices->at(0)->sptr);
  unsigned long key = hash_fn(key_str);
  off_t storage_offset;
  std::string val;

  // Check if key does not exist
  if (indices->at(0)->off_map->exists(key) == 0) {
    delete rec_ptr;
    return EXIT_SUCCESS;
  }

  storage_offset = indices->at(0)->off_map->at(key);
  val = tab->fs_data.at(storage_offset);

  before_rec = deserialize_to_record(val, tab->sptr, false);

  // Add log entry
  entry_stream.str("");
  entry_stream << st.transaction_id << " " << st.op_type << " " << st.table_id
               << " " << serialize(before_rec, before_rec->sptr, false) << "\n";

  entry_str = entry_stream.str();
  fs_log.push_back(entry_str);

  // Remove entry in indices
  for (index_itr = 0; index_itr < num_indices; index_itr++) {
    key_str = get_data(rec_ptr, indices->at(index_itr)->sptr);
    key = hash_fn(key_str);

    // Lazy deletion
    indices->at(index_itr)->off_map->erase(key);
  }

  before_rec->clear_data();
  delete before_rec;

  end: delete rec_ptr;
  return EXIT_SUCCESS;
}

int wal_engine::update(const statement& st) {
  LOG_INFO("Update");
  record* rec_ptr = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  plist<table_index*>* indices = db->tables->at(st.table_id)->indices;

  std::string key_str = get_data(rec_ptr, indices->at(0)->sptr);
  unsigned long key = hash_fn(key_str);
  off_t storage_offset;
  std::string val, before_tuple;
  record* before_rec = NULL;

  // Check if key does not exist
  if (indices->at(0)->off_map->exists(key) == 0) {
    delete rec_ptr;
    return EXIT_SUCCESS;
  }

  storage_offset = indices->at(0)->off_map->at(key);
  val = tab->fs_data.at(storage_offset);

  //LOG_INFO("val : %s", val.c_str());
  before_rec = deserialize_to_record(val, tab->sptr, false);
  //LOG_INFO("before tuple : %s", serialize(before_rec, tab->sptr, false).c_str());

  entry_stream.str("");
  entry_stream << st.transaction_id << " " << st.op_type << " " << st.table_id
               << " ";
  // before image
  //entry_stream << serialize(before_rec, before_rec->sptr) << " ";

  // Update existing record
  for (int field_itr : st.field_ids) {
    before_rec->set_data(field_itr, rec_ptr);
  }

  //LOG_INFO("update tuple : %s", serialize(before_rec, tab->sptr, false).c_str());

  // after image
  before_tuple = serialize(before_rec, tab->sptr, false);
  entry_stream << before_tuple << "\n";

  // Add log entry
  entry_str = entry_stream.str();
  fs_log.push_back(entry_str);

  // In-place update

  LOG_INFO("update offset : %lu", storage_offset);
  tab->fs_data.update(storage_offset, before_tuple);

  delete before_rec;

  end: delete rec_ptr;
  return EXIT_SUCCESS;
}

void wal_engine::txn_begin() {
}

void wal_engine::txn_end(bool commit) {
}

void wal_engine::recovery() {

  int op_type, txn_id, table_id;
  table *tab;
  plist<table_index*>* indices;
  unsigned int num_indices, index_itr;

  field_info finfo;
  std::string entry_str;
  std::ifstream log_file(fs_log.log_file_name);

  /*
   while (std::getline(log_file, entry_str)) {
   //LOG_INFO("entry : %s ", entry_str.c_str());
   std::stringstream entry(entry_str);

   entry >> txn_id >> op_type >> table_id;

   switch (op_type) {
   case operation_type::Insert: {
   LOG_INFO("Redo Insert");

   tab = db->tables->at(table_id);
   schema* sptr = tab->sptr;
   record* after_rec = deserialize(entry_str, sptr);

   indices = tab->indices;
   num_indices = tab->num_indices;

   tab->pm_data->push_back(after_rec);

   // Add entry in indices
   for (index_itr = 0; index_itr < num_indices; index_itr++) {
   std::string key_str = get_data(after_rec,
   indices->at(index_itr)->sptr);
   unsigned long key = hash_fn(key_str);

   indices->at(index_itr)->off_map->insert(key, after_rec);
   }
   }
   break;

   case operation_type::Delete: {
   LOG_INFO("Redo Delete");

   tab = db->tables->at(table_id);
   schema* sptr = tab->sptr;
   record* before_rec = deserialize(entry_str, sptr);

   indices = tab->indices;
   num_indices = tab->num_indices;

   tab->pm_data->erase(before_rec);

   // Remove entry in indices
   for (index_itr = 0; index_itr < num_indices; index_itr++) {
   std::string key_str = get_data(before_rec,
   indices->at(index_itr)->sptr);
   unsigned long key = hash_fn(key_str);

   indices->at(index_itr)->off_map->erase(key);
   }
   }
   break;

   case operation_type::Update:
   LOG_INFO("Redo Update");
   {
   int num_fields;
   int field_itr;

   tab = db->tables->at(table_id);
   schema* sptr = tab->sptr;
   record* before_rec = deserialize(entry_str, sptr);
   record* after_rec = deserialize(entry_str, sptr);

   // Update entry in indices
   for (index_itr = 0; index_itr < num_indices; index_itr++) {
   std::string key_str = get_data(before_rec,
   indices->at(index_itr)->sptr);
   unsigned long key = hash_fn(key_str);

   indices->at(index_itr)->off_map->insert(key, after_rec);
   }

   }

   break;

   default:
   cout << "Invalid operation type" << op_type << endl;
   break;
   }

   }
   */

}

