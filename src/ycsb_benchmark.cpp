// YCSB BENCHMARK

#include "ycsb_benchmark.h"

using namespace std;

#define DELIM ' '

class usertable_key : public key {
 public:
  usertable_key(unsigned int _id)
      : id(_id) {
  }

  std::string get_string() {
    return to_string(id);
  }

  unsigned int id;
};


class usertable_record : public record {
 public:
  usertable_record(usertable_key* _key, char* _value)
      : key_ptr(_key),
        value(_value) {
  }

  std::string get_string() {
    std::string data(key_ptr->get_string() + " " + value);
    return data;
  }

 private:
  usertable_key* key_ptr;
  char* value;
};

ycsb_benchmark::ycsb_benchmark(config& _conf)
    : conf(_conf),
      s_id(0) {

  // Usertable
  table* usertable = new table("usertable");
  load.tables["usertable"] = usertable;

  // Generate Zipf dist
  long part_range = conf.num_keys / conf.num_parts;
  long part_txns = conf.num_txns / conf.num_parts;

  zipf(zipf_dist, conf.skew, part_range, part_txns);
  uniform(uniform_dist, part_txns);

}

workload* ycsb_benchmark::get_dataset() {

  table* usertable_ptr = load.tables["usertable"];

  int part_range = conf.num_keys / conf.num_parts;
  int part_txns = conf.num_txns / conf.num_parts;
  int part_itr, txn_itr;

  for (int txn_itr = 0; txn_itr < part_txns; txn_itr++) {

    for (int part_itr = 0; part_itr < conf.num_parts; part_itr++) {
      unsigned int part_offset = part_range * part_itr;
      unsigned int key = part_offset + txn_itr;

      char* value = new char[conf.sz_value];
      random_string(value, conf.sz_value);

      usertable_key* key_ptr = new usertable_key(key);
      record* rec_ptr = new usertable_record(key_ptr, value);

      statement* st = new statement(++s_id, partition_type::Single, part_itr,
                                    operation_type::Insert, usertable_ptr,
                                    rec_ptr, key_ptr);
      vector<statement*> stmts = { st };

      transaction txn(stmts);
      load.txns.push_back(txn);
    }

  }

  cout << load.txns.size() << " dataset transactions " << endl;

  return &load;
}

workload* ycsb_benchmark::get_workload() {

  table* usertable_ptr = load.tables["usertable"];

  int part_range = conf.num_keys / conf.num_parts;
  int part_txns = conf.num_txns / conf.num_parts;
  int part_itr, txn_itr;

  for (int txn_itr = 0; txn_itr < part_txns; txn_itr++) {

    for (int part_itr = 0; part_itr < conf.num_parts; part_itr++) {
      unsigned int part_offset = part_range * part_itr;
      unsigned int key = part_offset + txn_itr;

      long z = conf.zipf_dist[key];
      double u = conf.uniform_dist[key];

      if (u < conf.per_writes) {
        char* updated_val = new char[conf.sz_value];
        memset(updated_val, 'x', conf.sz_value);
        updated_val[conf.sz_value - 1] = '\0';

        usertable_key* key_ptr = new usertable_key(key);
        record* rec_ptr = new usertable_record(key_ptr, updated_val);

        statement* st = new statement(++s_id, partition_type::Single, part_itr,
                                      operation_type::Update, usertable_ptr,
                                      rec_ptr, key_ptr);
        vector<statement*> stmts = { st };

        transaction txn(stmts);
        load.txns.push_back(txn);
      } else {

        usertable_key* key_ptr = new usertable_key(key);

        statement* st = new statement(++s_id, partition_type::Single, part_itr,
                                      operation_type::Select, usertable_ptr,
                                      NULL, key_ptr);
        vector<statement*> stmts = { st };

        transaction txn(stmts);
        load.txns.push_back(txn);
      }
    }

  }

  return &load;
}
