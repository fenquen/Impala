// (c) 2011 Cloudera, Inc. All rights reserved.

#include "runtime/plan-fragment-executor.h"

#include <Thrift.h>
#include <protocol/TBinaryProtocol.h>
#include <protocol/TDebugProtocol.h>
#include <transport/TBufferTransports.h>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/unordered_map.hpp>

#include "codegen/llvm-codegen.h"
#include "common/logging.h"
#include "common/object-pool.h"
#include "exec/data-sink.h"
#include "exec/exec-node.h"
#include "exec/scan-node.h"
#include "exec/hbase-table-scanner.h"
#include "exprs/expr.h"
#include "runtime/descriptors.h"
#include "runtime/data-stream-mgr.h"
#include "runtime/row-batch.h"
#include "util/cpu-info.h"
#include "util/debug-util.h"
#include "gen-cpp/ImpalaPlanService_types.h"

DEFINE_bool(serialize_batch, false, "serialize and deserialize each returned row batch");
DEFINE_int32(status_report_interval, 5, "interval between profile reports; in seconds");

using namespace std;
using namespace boost;
using namespace apache::thrift;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;

namespace impala {

PlanFragmentExecutor::PlanFragmentExecutor(
    ExecEnv* exec_env, const ReportStatusCallback& report_status_cb)
  : exec_env_(exec_env),
    report_status_cb_(report_status_cb),
    report_thread_active_(false),
    done_(false),
    prepared_(false) {
}

PlanFragmentExecutor::~PlanFragmentExecutor() {
  // delete row batch and free up resources, otherwise DiskIoMgr will complain
  // if we try to Close() an HdfsScanNode while still holding
  // DiskIoMgr::BufferDescriptors
  row_batch_.reset(NULL);
  // Prepare may not have been called, which sets runtime_state_
  if (runtime_state_.get() != NULL) {
    plan_->Close(runtime_state_.get());
    if (sink_.get() != NULL) {
      sink_->Close(runtime_state());
    }
  }
  // at this point, the report thread should have been stopped
  DCHECK(!report_thread_active_);
}

Status PlanFragmentExecutor::Prepare(
    const TPlanExecRequest& request, const TPlanExecParams& params) {
  query_id_ = request.query_id;

  VLOG_QUERY << "Prepare(): query_id=" << PrintId(query_id_)
             << " fragment_id=" << PrintId(request.fragment_id);
  VLOG(3) << "params:\n" << ThriftDebugString(params);

  runtime_state_.reset(
      new RuntimeState(request.fragment_id, request.query_options,
          request.query_globals.now_string, exec_env_));

  // set up desc tbl
  DescriptorTbl* desc_tbl = NULL;
  DCHECK(request.__isset.desc_tbl);
  RETURN_IF_ERROR(DescriptorTbl::Create(obj_pool(), request.desc_tbl, &desc_tbl));
  runtime_state_->set_desc_tbl(desc_tbl);
  VLOG_QUERY << "descriptor table for fragment=" << request.fragment_id << "\n"
             << desc_tbl->DebugString();

  // set up plan
  DCHECK(request.__isset.plan_fragment);
  RETURN_IF_ERROR(
      ExecNode::CreateTree(obj_pool(), request.plan_fragment, *desc_tbl, &plan_));
  runtime_state_->runtime_profile()->AddChild(plan_->runtime_profile());
  RETURN_IF_ERROR(plan_->Prepare(runtime_state_.get()));
  
  // set scan ranges
  vector<ExecNode*> scan_nodes;
  plan_->CollectScanNodes(&scan_nodes);
  for (int i = 0; i < scan_nodes.size(); ++i) {
    for (int j = 0; j < params.scan_ranges.size(); ++j) {
      if (scan_nodes[i]->id() == params.scan_ranges[j].nodeId) {
        RETURN_IF_ERROR(static_cast<ScanNode*>(
            scan_nodes[i])->SetScanRange(params.scan_ranges[j]));
      }
    }
  }

  if (VLOG_QUERY_IS_ON) PrintVolumeIds(params);

  // set up sink, if required  
  if (request.__isset.data_sink) {
    RETURN_IF_ERROR(DataSink::CreateDataSink(request, params, row_desc(), &sink_));
    RETURN_IF_ERROR(sink_->Init(runtime_state()));
  } else {
    sink_.reset(NULL);
  }

  // set up profile counters
  rows_produced_counter_ = ADD_COUNTER(runtime_state_->runtime_profile(), "RowsProduced",
      TCounterType::UNIT);

  row_batch_.reset(new RowBatch(plan_->row_desc(), runtime_state_->batch_size()));
  VLOG(3) << "plan_root=\n" << plan_->DebugString();
  prepared_ = true;
  return Status::OK;
}

void PlanFragmentExecutor::PrintVolumeIds(const TPlanExecParams& params) {
  // map from volume id to (# of splits, total # bytes) on that volume
  unordered_map<int32_t, pair<int, int64_t> > per_volume_stats;
  for (int i = 0; i < params.scan_ranges.size(); ++i) {
    const TScanRange& scan_range = params.scan_ranges[i];
    if (!scan_range.__isset.hdfsFileSplits) continue;
    for (int j = 0; j < scan_range.hdfsFileSplits.size(); ++j) {
      const THdfsFileSplit& split = scan_range.hdfsFileSplits[j];
      unordered_map<int32_t, pair<int, int64_t> >::iterator entry =
          per_volume_stats.find(split.volumeId);
      if (entry == per_volume_stats.end()) {
        entry = per_volume_stats.insert(make_pair(split.volumeId, make_pair(0, 0))).first;
      }
      pair<int, int64_t>& stats = entry->second;
      ++(stats.first);
      stats.second += split.length;
    }
  }

  stringstream str;
  str << "Hdfs split stats (<volume id>:<# splits>/<split lengths>) for query="
      << query_id_ << ":\n";
  for (unordered_map<int32_t, pair<int, int64_t> >::iterator i = per_volume_stats.begin();
       i != per_volume_stats.end(); ++i) {
     str << i->first << ":" << i->second.first << "/"
         << PrettyPrinter::Print(i->second.second, TCounterType::UNIT) << " ";
  }
  VLOG_QUERY << str.str();
}

Status PlanFragmentExecutor::Open() {
  VLOG_QUERY << "Open(): fragment_id=" << runtime_state_->fragment_id();
  // we need to start the profile-reporting thread before calling Open(), since it
  // may block
  // TODO: if no report thread is started, make sure to send a final profile
  // at end, otherwise the coordinator hangs in case we finish w/ an error
  if (!report_status_cb_.empty() && FLAGS_status_report_interval > 0) {
    unique_lock<mutex> l(report_thread_lock_);
    report_thread_ = thread(&PlanFragmentExecutor::ReportProfile, this);
    // make sure the thread started up, otherwise ReportProfile() might get into a race
    // with StopReportThread()
    report_thread_started_cv_.wait(l);
    report_thread_active_ = true;
  }

  Status status = OpenInternal();
  UpdateStatus(status);
  return status;
}

Status PlanFragmentExecutor::OpenInternal() {
  RETURN_IF_ERROR(plan_->Open(runtime_state_.get()));

  if (sink_.get() == NULL) return Status::OK;

  // If there is a sink, do all the work of driving it here, so that
  // when this returns the query has actually finished
  RowBatch* batch;
  while (true) {
    RETURN_IF_ERROR(GetNextInternal(&batch));
    if (batch == NULL) break;
    if (VLOG_ROW_IS_ON) {
      VLOG_ROW << "OpenInternal: #rows=" << batch->num_rows();
      for (int i = 0; i < batch->num_rows(); ++i) {
        TupleRow* row = batch->GetRow(i);
        VLOG_ROW << PrintRow(row, row_desc());
      }
    }
    COUNTER_UPDATE(rows_produced_counter_, batch->num_rows());
    RETURN_IF_ERROR(sink_->Send(runtime_state(), batch));
  }

  // Close the sink *before* stopping the report thread. Close may
  // need to add some important information to the last report that
  // gets sent. (e.g. table sinks record the files they have written
  // to in this method)
  // The coordinator report channel waits until all backends are
  // either in error or have returned a status report with done =
  // true, so tearing down any data stream state (a separate
  // channel) in Close is safe.

  // TODO: If this returns an error, the d'tor will call Close again. We should
  // audit the sinks to check that this is ok, or change that behaviour. 
  RETURN_IF_ERROR(sink_->Close(runtime_state()));

  // Setting to NULL ensures that the d'tor won't double-close the sink. 
  sink_.reset(NULL);
  done_ = true;

  StopReportThread();
  SendReport(true);

  return Status::OK;
}

void PlanFragmentExecutor::ReportProfile() {
  VLOG_QUERY << "ReportProfile(): fragment_id=" << runtime_state_->fragment_id();
  DCHECK(!report_status_cb_.empty());
  unique_lock<mutex> l(report_thread_lock_);
  // tell Open() that we started
  report_thread_started_cv_.notify_one();

  while (true) {
    system_time timeout =
        get_system_time() + posix_time::seconds(FLAGS_status_report_interval);
    bool notified = stop_report_thread_cv_.timed_wait(l, timeout);
    VLOG_FILE << "Reporting " << (notified ? "final " : " ")
              << "profile for fragment " << runtime_state_->fragment_id();
    if (VLOG_QUERY_IS_ON) {
      stringstream ss;
      profile()->PrettyPrint(&ss);
      VLOG_QUERY << ss.str();
    }

    if (notified) {
      VLOG_QUERY << "exiting reporting thread: fragment_id="
                 << runtime_state_->fragment_id();
      return;
    } else {
      SendReport(false);
    }
  }
}

void PlanFragmentExecutor::SendReport(bool done) {
  if (report_status_cb_.empty()) return;

  Status status;
  {
    lock_guard<mutex> l(status_lock_);
    status = status_;
  }
  // don't send a final report if we got cancelled, nobody's going to look at it
  // anyway
  if (!status.IsCancelled()) {
    // notified means someone notified on stop_report_thread_cv_
    report_status_cb_(status, profile(), done || !status.ok());
  }
}

void PlanFragmentExecutor::StopReportThread() {
  if (!report_thread_active_) return;
  {
    lock_guard<mutex> l(report_thread_lock_);
    // notify with lock held: ReportProfile() does a timed_wait() on the cond
    // var, we need to make sure it really is sitting in timed_wait() while
    // we notify()
    stop_report_thread_cv_.notify_one();
  }
  report_thread_.join();
  report_thread_active_ = false;
}

Status PlanFragmentExecutor::GetNext(RowBatch** batch) {
  VLOG_FILE << "GetNext(): fragment_id=" << runtime_state_->fragment_id();
  Status status = GetNextInternal(batch);
  UpdateStatus(status);
  if (done_) {
    VLOG_QUERY << "Finished executing fragment query_id=" << PrintId(query_id_)
               << " fragment_id=" << PrintId(runtime_state_->fragment_id());
    StopReportThread();
    SendReport(true);
  }
  return status;
}

Status PlanFragmentExecutor::GetNextInternal(RowBatch** batch) {
  if (done_) {
    *batch = NULL;
    return Status::OK;
  }

  while (!done_) {
    row_batch_->Reset();
    RETURN_IF_ERROR(plan_->GetNext(runtime_state_.get(), row_batch_.get(), &done_));
    if (row_batch_->num_rows() > 0) {
      COUNTER_UPDATE(rows_produced_counter_, row_batch_->num_rows());
      *batch = row_batch_.get();
      break;
    }
    *batch = NULL;
  }

  return Status::OK;
}

void PlanFragmentExecutor::UpdateStatus(const Status& status) {
  if (status.ok()) return;
  {
    lock_guard<mutex> l(status_lock_);
    if (status_.ok()) status_ = status;
  }
  StopReportThread();
  SendReport(true);
}

void PlanFragmentExecutor::Cancel() {
  VLOG_QUERY << "Cancel(): fragment_id=" << runtime_state_->fragment_id();
  DCHECK(prepared_);
  runtime_state_->set_is_cancelled(true);
  runtime_state_->stream_mgr()->Cancel(runtime_state_->fragment_id());
}

const RowDescriptor& PlanFragmentExecutor::row_desc() {
  return plan_->row_desc();
}

RuntimeProfile* PlanFragmentExecutor::profile() {
  return runtime_state_->runtime_profile();
}

}
