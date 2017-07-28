#ifndef DIALOG_DIALOG_STORE_H_
#define DIALOG_DIALOG_STORE_H_

#include <math.h>

#include <functional>
#include <numeric>
#include <thread>

#include "storage.h"
#include "monolog.h"
#include "read_tail.h"
#include "schema.h"
#include "string_map.h"
#include "table_metadata.h"
#include "data_log.h"
#include "index_log.h"
#include "filter_log.h"
#include "alert_log.h"
#include "task_worker.h"
#include "periodic_task.h"
#include "expression_compiler.h"
#include "query_planner.h"
#include "record_stream.h"
#include "radix_tree.h"
#include "filter.h"
#include "trigger.h"
#include "trigger_parser.h"
#include "exceptions.h"

#include "optional.h"

#include "time_utils.h"
#include "string_utils.h"
#include "task_pool.h"

using namespace ::dialog::monolog;
using namespace ::dialog::index;
using namespace ::dialog::monitor;
using namespace ::utils;

// TODO: Add more tests
// TODO: Improve documentation

namespace dialog {

class dialog_table {
 public:
  typedef data_log data_log_type;
  typedef schema_t schema_type;
  typedef read_tail read_tail_type;
  typedef metadata_writer metadata_writer_type;

  typedef size_t filter_id_t;
  typedef std::pair<size_t, size_t> trigger_id_t;

  typedef radix_index::rt_result ri_offset_list;
  typedef record_stream<ri_offset_list> ri_stream_type;
  typedef filtered_record_stream<ri_stream_type> fri_rstream_type;
  typedef union_record_stream<fri_rstream_type> fri_result_type;

  typedef filter::range_result filter_offset_list;
  typedef record_stream<filter_offset_list> filter_rstream_type;
  typedef filtered_record_stream<filter_rstream_type> ffilter_rstream_type;

  dialog_table(const std::vector<column_t>& table_schema,
               const std::string& path, const storage::storage_mode& storage,
               task_pool& pool)
      : data_log_("data_log", path, storage),
        rt_(path, storage),
        schema_(path, table_schema),
        metadata_(path, storage.id),
        mgmt_pool_(pool),
        monitor_task_("monitor") {
    monitor_task_.start([this]() {
      uint64_t cur_ts = time_utils::cur_ns();
      uint64_t cur_ts_block = filter::get_ts_block(cur_ts);
      uint64_t version = rt_.get();
      size_t nfilters = filters_.size();
      for (size_t i = 0; i < nfilters; i++) {
        // FIXME: Hard-coded, should change to user specified/dirty bit based
        filter* f = filters_.at(i);
        if (f->is_valid()) {
          for (size_t ts_block = cur_ts_block - 10; ts_block <= cur_ts_block + 10; ts_block++) {
            const aggregated_reflog* ar = f->lookup(ts_block);
            size_t ntriggers = f->num_triggers();
            for (size_t tid = 0; tid < ntriggers; tid++) {
              trigger* t = f->get_trigger(tid);
              if (t->is_valid() && ar != nullptr) {
                numeric agg = ar->get_aggregate(tid, version);
                if (numeric::relop(t->op(), agg, t->threshold())) {
                  alerts_.insert(alert(ts_block, t, agg, version));
                }
              }
            }
          }
        }
      }
    });
  }

  // Management ops
  void add_index(const std::string& field_name, double bucket_size = 1.0) {
    optional<management_exception> ex;
    auto ret =
        mgmt_pool_.submit(
            [field_name, bucket_size, &ex, this] {

              uint16_t idx;
              try {
                idx = schema_.get_field_index(field_name);
              } catch (std::exception& e) {
                ex = management_exception("Could not add index for " + field_name + " : " + e.what());
                return;
              }

              column_t& col = schema_[idx];
              bool success = col.set_indexing();
              if (success) {
                uint16_t index_id = UINT16_MAX;
                switch (col.type().id) {
                  case type_id::D_BOOL:
                  index_id = indexes_.push_back(new radix_index(1, 2));
                  break;
                  case type_id::D_CHAR:
                  case type_id::D_SHORT:
                  case type_id::D_INT:
                  case type_id::D_LONG:
                  case type_id::D_FLOAT:
                  case type_id::D_DOUBLE:
                  case type_id::D_STRING:
                  index_id = indexes_.push_back(new radix_index(col.type().size, 256));
                  break;
                  default:
                  col.set_unindexed();
                  ex = management_exception("Index not supported for field type");
                  return;
                }
                col.set_indexed(index_id, bucket_size);
                metadata_.write_index_info(field_name, bucket_size);
              } else {
                ex = management_exception("Could not index " + field_name + ": already indexed/indexing");
                return;
              }
            });

    ret.wait();
    if (ex.has_value())
      throw ex.value();
  }

  void remove_index(const std::string& field_name) {
    optional<management_exception> ex;
    auto ret =
        mgmt_pool_.submit(
            [field_name, &ex, this] {
              uint16_t idx;
              try {
                idx = schema_.get_field_index(field_name);
              } catch (std::exception& e) {
                ex = management_exception("Could not remove index for " + field_name + " : " + e.what());
                return;
              }

              if (!schema_[idx].disable_indexing()) {
                ex = management_exception("Could not remove index for " + field_name + ": No index exists");
                return;
              }
            });
    ret.wait();
    if (ex.has_value())
      throw ex.value();
  }

  void add_filter(const std::string& filter_name,
                  const std::string& expression) {
    optional<management_exception> ex;
    auto ret =
        mgmt_pool_.submit(
            [filter_name, expression, &ex, this] {
              filter_id_t filter_id;
              if (filter_map_.get(filter_name, filter_id)) {
                ex = management_exception("Filter " + filter_name + " already exists.");
                return;
              }
              auto cexpr = expression_compiler::compile(expression, schema_);
              filter_id = filters_.push_back(new filter(cexpr, default_filter));
              metadata_.write_filter_info(filter_name, expression);
              bool success = filter_map_.put(filter_name, filter_id);
              if (!success) {
                ex = management_exception("Could not add filter " + filter_name + " to filter map.");
                return;
              }
            });
    ret.wait();
    if (ex.has_value())
      throw ex.value();
  }

  void remove_filter(const std::string& filter_name) {
    optional<management_exception> ex;
    auto ret = mgmt_pool_.submit([filter_name, &ex, this] {
      filter_id_t filter_id;
      if (!filter_map_.get(filter_name, filter_id)) {
        ex = management_exception("Filter " + filter_name + " does not exist.");
        return;
      }
      bool success = filters_.at(filter_id)->invalidate();
      if (!success) {
        ex = management_exception("Filter already invalidated.");
        return;
      }
    });
    ret.wait();
    if (ex.has_value())
      throw ex.value();
  }

  void add_trigger(const std::string& trigger_name,
                   const std::string& filter_name,
                   const std::string& trigger_expression) {
    optional<management_exception> ex;
    auto ret =
        mgmt_pool_.submit(
            [trigger_name, filter_name, trigger_expression, &ex, this] {
              trigger_id_t trigger_id;
              if (trigger_map_.get(trigger_name, trigger_id)) {
                ex = management_exception("Trigger " + trigger_name + " already exists.");
                return;
              }
              filter_id_t filter_id;
              if (!filter_map_.get(filter_name, filter_id)) {
                ex = management_exception("Filter " + filter_name + " does not exist.");
                return;
              }
              trigger_id.first = filter_id;
              trigger_parser parser(trigger_expression, schema_);
              parsed_trigger tp = parser.parse();
              const column_t& col = schema_[tp.field_name];
              trigger* t = new trigger(trigger_name, filter_name, tp.agg, col.name(), col.idx(), col.type(), tp.relop, tp.threshold);
              trigger_id.second = filters_.at(filter_id)->add_trigger(t);
              metadata_.write_trigger_info(trigger_name, filter_name, tp.agg, tp.field_name, tp.relop,
                  tp.threshold);
            });
    ret.wait();
    if (ex.has_value())
      throw ex.value();
  }

  void remove_trigger(const std::string& trigger_name) {
    optional<management_exception> ex;
    auto ret =
        mgmt_pool_.submit(
            [trigger_name, &ex, this] {
              trigger_id_t trigger_id;
              if (!trigger_map_.get(trigger_name, trigger_id)) {
                ex = management_exception("Trigger " + trigger_name + " does not exist.");
                return;
              }
              bool success = filters_.at(trigger_id.first)->remove_trigger(trigger_id.second);
              if (!success) {
                ex = management_exception("Trigger already invalidated.");
                return;
              }
            });
    ret.wait();
    if (ex.has_value())
      throw ex.value();
  }

  // Query ops
  uint64_t append(void* data) {
    size_t record_size = schema_.record_size();
    uint64_t offset = data_log_.append((const uint8_t*) data, record_size);
    record_t r = schema_.apply(offset, data_log_.ptr(offset));

    size_t nfilters = filters_.size();
    for (size_t i = 0; i < nfilters; i++)
      if (filters_.at(i)->is_valid())
        filters_.at(i)->update(r);

    for (const field_t& f : r) {
      if (f.is_indexed()) {
        indexes_.at(f.index_id())->insert(f.get_key(), offset);
      }
    }

    data_log_.flush(offset, record_size);
    rt_.advance(offset, record_size);
    return offset;
  }

  bool read(uint64_t offset, record_t& rec) const {
    uint64_t version = rt_.get();
    if (offset < version) {
      rec = record_t(offset, data_log_.cptr(offset), schema_.record_size());
      return true;
    }
    return false;
  }

  fri_result_type execute_filter(const std::string& expr) const {
    uint64_t version = rt_.get();
    compiled_expression cexpr = expression_compiler::compile(expr, schema_);
    query_planner planner(cexpr, indexes_);
    query_plan plan = planner.plan();
    std::vector<fri_rstream_type> fstreams;
    for (minterm_plan& mplan : plan) {
      const index_filter& f = mplan.idx_filter();
      auto mres = indexes_.at(f.index_id())->range_lookup(f.kbegin(), f.kend());
      ri_stream_type rs(version, mres, schema_, data_log_);
      fstreams.push_back(fri_rstream_type(rs, mplan.data_filter()));
    }
    return fri_result_type(fstreams);
  }

  filter_rstream_type query_filter(const std::string& filter_name,
                                   uint64_t ts_block_begin,
                                   uint64_t ts_block_end) const {

    size_t filter_id;
    if (!filter_map_.get(filter_name, filter_id)) {
      THROW(invalid_operation_exception,
            "Filter " + filter_name + " does not exist.");
    }
    auto res = filters_.at(filter_id)->lookup_range(ts_block_begin,
                                                    ts_block_end);
    return filter_rstream_type(rt_.get(), res, schema_, data_log_);
  }

  ffilter_rstream_type query_filter(const std::string& filter_name,
                                    const std::string& expr,
                                    uint64_t ts_block_begin,
                                    uint64_t ts_block_end) const {
    return ffilter_rstream_type(
        query_filter(filter_name, ts_block_begin, ts_block_end),
        expression_compiler::compile(expr, schema_));
  }

  const std::set<alert>& get_alerts() const {
    return alerts_;
  }

  size_t num_records() const {
    return rt_.get();
  }

 protected:
  data_log_type data_log_;
  read_tail_type rt_;
  schema_type schema_;
  metadata_writer_type metadata_;

  // In memory structures
  filter_log filters_;
  index_log indexes_;
  std::set<alert> alerts_;

  string_map<filter_id_t> filter_map_;
  string_map<trigger_id_t> trigger_map_;

  // Manangement
  task_pool& mgmt_pool_;
  periodic_task monitor_task_;
};

}

#endif /* DIALOG_DIALOG_STORE_H_ */
