// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once
#include <brpc/controller.h>
#include <bthread/types.h>
#include <butil/errno.h>
#include <fmt/format.h>
#include <gen_cpp/PaloInternalService_types.h>
#include <gen_cpp/Types_types.h>
#include <gen_cpp/internal_service.pb.h>
#include <gen_cpp/types.pb.h>
#include <glog/logging.h>
#include <google/protobuf/stubs/callback.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>
// IWYU pragma: no_include <bits/chrono.h>
#include <chrono> // IWYU pragma: keep
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/config.h"
#include "common/status.h"
#include "exec/data_sink.h"
#include "exec/tablet_info.h"
#include "gutil/ref_counted.h"
#include "runtime/exec_env.h"
#include "runtime/memory/mem_tracker.h"
#include "runtime/thread_context.h"
#include "runtime/types.h"
#include "util/countdown_latch.h"
#include "util/runtime_profile.h"
#include "util/stopwatch.hpp"
#include "vec/columns/column.h"
#include "vec/common/allocator.h"
#include "vec/common/hash_table/phmap_fwd_decl.h"
#include "vec/core/block.h"
#include "vec/data_types/data_type.h"
#include "vec/exprs/vexpr_fwd.h"
#include "vec/sink/vrow_distribution.h"

namespace doris {
class DeltaWriterV2;
class LoadStreamStub;
class ObjectPool;
class RowDescriptor;
class RuntimeState;
class TDataSink;
class TExpr;
class TabletSchema;
class TupleDescriptor;

namespace stream_load {
class LoadStreams;
}

namespace vectorized {

class OlapTableBlockConvertor;
class OlapTabletFinder;
class VOlapTableSinkV2;
class DeltaWriterV2Map;

using Streams = std::vector<std::shared_ptr<LoadStreamStub>>;

struct Rows {
    int64_t partition_id;
    int64_t index_id;
    std::vector<int32_t> row_idxes;
};

using RowsForTablet = std::unordered_map<int64_t, Rows>;

// Write block data to Olap Table.
// When OlapTableSink::open() called, there will be a consumer thread running in the background.
// When you call VOlapTableSinkV2::send(), you will be the producer who products pending batches.
// Join the consumer thread in close().
class VOlapTableSinkV2 final : public DataSink {
public:
    // Construct from thrift struct which is generated by FE.
    VOlapTableSinkV2(ObjectPool* pool, const RowDescriptor& row_desc,
                     const std::vector<TExpr>& texprs, Status* status);

    ~VOlapTableSinkV2() override;

    Status init(const TDataSink& sink) override;
    // TODO: unify the code of prepare/open/close with result sink
    Status prepare(RuntimeState* state) override;

    Status open(RuntimeState* state) override;

    Status close(RuntimeState* state, Status close_status) override;

    Status send(RuntimeState* state, vectorized::Block* block, bool eos = false) override;

    Status on_partitions_created(TCreatePartitionResult* result);

private:
    Status _init_row_distribution();

    Status _open_streams(int64_t src_id);

    Status _open_streams_to_backend(int64_t dst_id, ::doris::stream_load::LoadStreams& streams);

    Status _incremental_open_streams(const std::vector<TOlapTablePartition>& partitions);

    void _build_tablet_node_mapping();

    void _generate_rows_for_tablet(std::vector<RowPartTabletIds>& row_part_tablet_ids,
                                   RowsForTablet& rows_for_tablet);

    Status _write_memtable(std::shared_ptr<vectorized::Block> block, int64_t tablet_id,
                           const Rows& rows, const Streams& streams);

    Status _select_streams(int64_t tablet_id, int64_t partition_id, int64_t index_id,
                           Streams& streams);

    Status _close_load(const Streams& streams);

    Status _cancel(Status status);

    std::shared_ptr<MemTracker> _mem_tracker;

    ObjectPool* _pool;

    // unique load id
    PUniqueId _load_id;
    int64_t _txn_id = -1;
    int _num_replicas = -1;
    int _tuple_desc_id = -1;

    // this is tuple descriptor of destination OLAP table
    TupleDescriptor* _output_tuple_desc = nullptr;
    RowDescriptor* _output_row_desc = nullptr;

    // number of senders used to insert into OlapTable, if we only support single node insert,
    // all data from select should collectted and then send to OlapTable.
    // To support multiple senders, we maintain a channel for each sender.
    int _sender_id = -1;
    int _num_senders = -1;
    int64_t _backend_id = -1;
    int _stream_per_node = -1;
    int _total_streams = -1;
    int _num_local_sink = -1;
    bool _is_high_priority = false;
    bool _write_file_cache = false;

    // TODO(zc): think about cache this data
    std::shared_ptr<OlapTableSchemaParam> _schema;
    OlapTableLocationParam* _location = nullptr;
    DorisNodesInfo* _nodes_info = nullptr;

    std::unique_ptr<OlapTabletFinder> _tablet_finder;

    std::unique_ptr<OlapTableBlockConvertor> _block_convertor;

    // Stats for this
    int64_t _send_data_ns = 0;
    int64_t _number_input_rows = 0;
    int64_t _number_output_rows = 0;

    MonotonicStopWatch _row_distribution_watch;

    RuntimeProfile::Counter* _input_rows_counter = nullptr;
    RuntimeProfile::Counter* _output_rows_counter = nullptr;
    RuntimeProfile::Counter* _filtered_rows_counter = nullptr;
    RuntimeProfile::Counter* _send_data_timer = nullptr;
    RuntimeProfile::Counter* _row_distribution_timer = nullptr;
    RuntimeProfile::Counter* _write_memtable_timer = nullptr;
    RuntimeProfile::Counter* _wait_mem_limit_timer = nullptr;
    RuntimeProfile::Counter* _validate_data_timer = nullptr;
    RuntimeProfile::Counter* _open_timer = nullptr;
    RuntimeProfile::Counter* _close_timer = nullptr;
    RuntimeProfile::Counter* _close_writer_timer = nullptr;
    RuntimeProfile::Counter* _close_load_timer = nullptr;
    RuntimeProfile::Counter* _add_partition_request_timer = nullptr;

    // Save the status of close() method
    Status _close_status;

    VOlapTablePartitionParam* _vpartition = nullptr;
    vectorized::VExprContextSPtrs _output_vexpr_ctxs;

    RuntimeState* _state = nullptr;

    std::unordered_set<int64_t> _opened_partitions;

    std::unordered_map<int64_t, std::unordered_map<int64_t, PTabletID>> _tablets_for_node;
    std::unordered_map<int64_t, std::vector<PTabletID>> _indexes_from_node;

    std::unordered_map<int64_t, std::shared_ptr<::doris::stream_load::LoadStreams>>
            _streams_for_node;

    size_t _stream_index = 0;
    std::shared_ptr<DeltaWriterV2Map> _delta_writer_for_tablet;

    VRowDistribution _row_distribution;
    // reuse to avoid frequent memory allocation and release.
    std::vector<RowPartTabletIds> _row_part_tablet_ids;
};

} // namespace vectorized
} // namespace doris
