// Copyright (c) 2012, Cloudera, inc.
//
// This utility will first try to load the data from the given path if the
// tablet doesn't already exist at the given location. It will then run
// the tpch1 query, as described below, up to tpch_num_query_iterations times.
//
// The input data must be in the tpch format, separated by "|".
//
// Usage:
//   tpch1 -tpch_path_to_data=/home/jdcryans/lineitem.tbl
//         -tpch_num_query_iterations=1
//         -tpch_expected_matching_rows=12345
//
// From Impala:
// ====
// ---- QUERY : TPCH-Q1
// # Q1 - Pricing Summary Report Query
// # Modifications: Remove ORDER BY, added ROUND() calls
// select
//   l_returnflag,
//   l_linestatus,
//   round(sum(l_quantity), 1),
//   round(sum(l_extendedprice), 1),
//   round(sum(l_extendedprice * (1 - l_discount)), 1),
//   round(sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)), 1),
//   round(avg(l_quantity), 1),
//   round(avg(l_extendedprice), 1),
//   round(avg(l_discount), 1), count(1)
// from
//   lineitem
// where
//   l_shipdate<='1998-09-02'
// group by
//   l_returnflag,
//   l_linestatus
// ---- TYPES
// string, string, double, double, double, double, double, double, double, bigint
// ---- RESULTS
// 'A','F',37734107,56586554400.7,53758257134.9,55909065222.8,25.5,38273.1,0,1478493
// 'N','F',991417,1487504710.4,1413082168.1,1469649223.2,25.5,38284.5,0.1,38854
// 'N','O',74476040,111701729697.7,106118230307.6,110367043872.5,25.5,38249.1,0,2920374
// 'R','F',37719753,56568041380.9,53741292684.6,55889619119.8,25.5,38250.9,0.1,1478870
// ====
#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <tr1/unordered_map>
#include <stdlib.h>

#include <glog/logging.h>

#include "kudu/benchmarks/tpch/tpch-schemas.h"
#include "kudu/benchmarks/tpch/rpc_line_item_dao.h"
#include "kudu/benchmarks/tpch/line_item_tsv_importer.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/hash/city.h"
#include "kudu/gutil/strings/numbers.h"
#include "kudu/integration-tests/mini_cluster.h"
#include "kudu/master/mini_master.h"
#include "kudu/util/env.h"
#include "kudu/util/slice.h"
#include "kudu/util/stopwatch.h"

DEFINE_string(tpch_path_to_data, "/tmp/lineitem.tbl",
              "The full path to the '|' separated file containing the lineitem table.");
DEFINE_int32(tpch_num_query_iterations, 1, "Number of times the query will be run.");
DEFINE_int32(tpch_expected_matching_rows, 5916591, "Number of rows that should match the query.");
DEFINE_bool(use_mini_cluster, true,
            "Create a mini cluster for the work to be performed against.");
DEFINE_string(mini_cluster_base_dir, "/tmp/tpch",
              "If using a mini cluster, directory for master/ts data.");
DEFINE_string(master_address, "localhost",
              "Address of master for the cluster to operate on");
DEFINE_int32(tpch_max_batch_size, 1000,
             "Maximum number of inserts/updates to batch at once");

namespace kudu {

using client::KuduColumnRangePredicate;
using client::KuduColumnSchema;
using client::KuduRowResult;
using client::KuduSchema;

using std::tr1::unordered_map;

struct Result {
  uint32_t l_quantity;
  uint32_t l_extendedprice;
  uint32_t l_discount;
  uint32_t l_tax;
  int count;
  Result()
    : l_quantity(0), l_extendedprice(0), l_discount(0), l_tax(0), count(0) {
  }
};

// This struct is used for the keys while running the GROUP BY instead of manipulating strings
struct SliceMapKey {
  Slice slice;

  // This copies the string out of the result buffer
  void RelocateSlice() {
    size_t size = slice.size();
    uint8_t *buf = new uint8_t[size];
    memcpy(buf, slice.data(), size);
    slice = Slice(buf, size);
  }

  bool operator==(const SliceMapKey &other_key) const {
    return slice == other_key.slice;
  }
};

struct hash {
  size_t operator()(const SliceMapKey &key) const {
    return util_hash::CityHash64(
      reinterpret_cast<const char *>(key.slice.data()), key.slice.size());
  }
};

void LoadLineItems(const string &path, RpcLineItemDAO *dao) {
  LineItemTsvImporter importer(path);

  while (importer.HasNextLine()) {
    dao->WriteLine(boost::bind(&LineItemTsvImporter::GetNextLine,
                               &importer, _1));
  }
  dao->FinishWriting();
}

void Tpch1(RpcLineItemDAO *dao) {
  typedef unordered_map<SliceMapKey, Result*, hash> slice_map;
  typedef unordered_map<SliceMapKey, slice_map*, hash> slice_map_map;

  KuduSchema query_schema(tpch::CreateTpch1QuerySchema());
  Slice date("1998-09-02");
  vector<KuduColumnRangePredicate> preds;
  KuduColumnRangePredicate pred1(query_schema.Column(0), NULL, &date);
  preds.push_back(pred1);

  dao->OpenScanner(query_schema, preds);

  int matching_rows = 0;
  slice_map_map results;
  Result *r;
  vector<KuduRowResult> rows;
  while (dao->HasMore()) {
    dao->GetNext(&rows);
    BOOST_FOREACH(const KuduRowResult& row, rows) {
      matching_rows++;

      SliceMapKey l_returnflag;
      CHECK_OK(row.GetString(1, &l_returnflag.slice));
      SliceMapKey l_linestatus;
      CHECK_OK(row.GetString(2, &l_linestatus.slice));
      uint32_t l_quantity;
      CHECK_OK(row.GetUInt32(3, &l_quantity));
      uint32_t l_extendedprice;
      CHECK_OK(row.GetUInt32(4, &l_extendedprice));
      uint32_t l_discount;
      CHECK_OK(row.GetUInt32(5, &l_discount));
      uint32_t l_tax;
      CHECK_OK(row.GetUInt32(6, &l_tax));

      slice_map *linestatus_map;
      slice_map_map::iterator it = results.find(l_returnflag);
      if (it == results.end()) {
        linestatus_map = new slice_map;
        l_returnflag.RelocateSlice();
        results[l_returnflag] = linestatus_map;
      } else {
        linestatus_map = it->second;
      }

      slice_map::iterator inner_it = linestatus_map->find(l_linestatus);
      if (inner_it == linestatus_map->end()) {
        r = new Result();
        l_linestatus.RelocateSlice();
        (*linestatus_map)[l_linestatus] = r;
      } else {
        r = inner_it->second;
      }
      r->l_quantity += l_quantity;
      r->l_extendedprice += l_extendedprice;
      r->l_discount += l_discount;
      r->l_tax += l_tax;
      r->count++;
    }
    rows.clear();
  }
  LOG(INFO) << "Result: ";
  for (slice_map_map::iterator ii = results.begin();
       ii != results.end(); ++ii) {
    const SliceMapKey returnflag = ii->first;
    slice_map *maps = ii->second;
    for (slice_map::iterator jj = maps->begin(); jj != maps->end(); ++jj) {
      const SliceMapKey linestatus = jj->first;
      Result *r = jj->second;
      double avg_q = static_cast<double>(r->l_quantity) / r->count;
      double avg_ext_p = r->l_extendedprice / r->count / 100.0;
      double avg_discount = r->l_discount / r->count / 100.0;
      LOG(INFO) << returnflag.slice.ToString() << ", " <<
                   linestatus.slice.ToString() << ", " <<
                   r->l_quantity << ", " <<
                   (r->l_extendedprice / 100.0) << ", " <<
                   // TODO those two are missing at the moment, might want to chagne Result
                   // sum(l_extendedprice * (1 - l_discount))
                   // sum(l_extendedprice * (1 - l_discount) * (1 + l_tax))
                   avg_q << ", " <<
                   avg_ext_p << ", " <<
                   avg_discount << ", " <<
                   r->count;
      delete r;
      delete linestatus.slice.data();
    }
    delete maps;
    delete returnflag.slice.data();
  }
  CHECK_EQ(matching_rows, FLAGS_tpch_expected_matching_rows) << "Wrong number of rows returned";
}

} // namespace kudu

int main(int argc, char **argv) {
  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, true);
  const char * const kTableName = "tpch1";

  gscoped_ptr<kudu::Env> env;
  gscoped_ptr<kudu::MiniCluster> cluster;
  string master_address;
  if (FLAGS_use_mini_cluster) {
    env.reset(new kudu::EnvWrapper(kudu::Env::Default()));
    kudu::Status s = env->CreateDir(FLAGS_mini_cluster_base_dir);
    CHECK(s.IsAlreadyPresent() || s.ok());
    cluster.reset(new kudu::MiniCluster(env.get(), FLAGS_mini_cluster_base_dir, 1));
    CHECK_OK(cluster->StartSync());
    master_address = cluster->mini_master()->bound_rpc_addr().ToString();
  } else {
    master_address = FLAGS_master_address;
  }

  gscoped_ptr<kudu::RpcLineItemDAO> dao(new kudu::RpcLineItemDAO(master_address, kTableName,
                                                                 FLAGS_tpch_max_batch_size));
  dao->Init();

  bool needs_loading = dao->IsTableEmpty();
  if (needs_loading) {
    LOG_TIMING(INFO, "loading") {
      kudu::LoadLineItems(FLAGS_tpch_path_to_data, dao.get());
    }
  } else {
    LOG(INFO) << "Data already in place";
  }
  for (int i = 0; i < FLAGS_tpch_num_query_iterations; i++) {
    LOG_TIMING(INFO, StringPrintf("querying for iteration # %d", i)) {
      kudu::Tpch1(dao.get());
    }
  }

  if (cluster) {
    cluster->Shutdown();
  }
  return 0;
}