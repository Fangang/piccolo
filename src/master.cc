#include "piccolo/master.h"
#include "piccolo/table.h"

#include "util/common.h"
#include "util/tuple.h"
#include "util/static-initializers.h"

#include <set>

using std::map;
using std::vector;
using std::set;

DECLARE_bool(work_stealing);
DECLARE_double(sleep_time);

namespace piccolo {

static std::set<int> dead_workers;

struct Taskid {
  int table;
  int shard;

  Taskid(int t, int s) :
      table(t), shard(s) {
  }

  bool operator<(const Taskid& b) const {
    return table < b.table || (table == b.table && shard < b.shard);
  }
};

struct TaskState: private boost::noncopyable {
  enum Status {
    PENDING = 0, ACTIVE = 1, FINISHED = 2
  };

  TaskState(Taskid id, int64_t size) :
      id(id), status(PENDING), size(size), stolen(false) {
  }

  static bool IdCompare(TaskState *a, TaskState *b) {
    return a->id < b->id;
  }

  static bool WeightCompare(TaskState *a, TaskState *b) {
    if (a->stolen && !b->stolen) {
      return true;
    }
    return a->size < b->size;
  }

  Taskid id;
  int status;
  int size;
  bool stolen;
};

typedef map<Taskid, TaskState*> TaskMap;
typedef std::set<Taskid> ShardSet;
struct WorkerState: private boost::noncopyable {
  WorkerState(int w_id) :
      id(w_id) {
    last_ping_time = Now();
    last_task_start = 0;
    total_runtime = 0;
    checkpointing = false;
  }

  TaskMap work;

  // Table shards this worker is responsible for serving.
  ShardSet shards;

  double last_ping_time;

  int id;

  double last_task_start;
  double total_runtime;

  bool checkpointing;

  // Order by number of pending tasks and last update time.
  static bool PendingCompare(WorkerState *a, WorkerState* b) {
//    return (a->pending_size() < b->pending_size());
    return a->num_pending() < b->num_pending();
  }

  bool alive() const {
    return dead_workers.find(id) == dead_workers.end();
  }

  bool is_assigned(Taskid id) {
    return work.find(id) != work.end();
  }

  void ping() {
    last_ping_time = Now();
  }

  double idle_time() {
    // Wait a little while before stealing work; should really be
    // using something like the standard deviation, but this works
    // for now.
    if (num_finished() != work.size())
      return 0;

    return Now() - last_ping_time;
  }

  void assign_shard(int shard, bool should_service) {
    TableRegistry::Map &tables = TableRegistry::tables();
    for (TableRegistry::Map::iterator i = tables.begin(); i != tables.end();
        ++i) {
      if (shard < i->second->numShards) {
        Taskid t(i->first, shard);
        if (should_service) {
          shards.insert(t);
        } else {
          shards.erase(shards.find(t));
        }
      }
    }
  }

  bool serves(Taskid id) const {
    return shards.find(id) != shards.end();
  }

  void assign_task(TaskState *s) {
    work[s->id] = s;
  }

  void remove_task(TaskState* s) {
    work.erase(work.find(s->id));
  }

  void clear_tasks() {
    work.clear();
  }

  void set_finished(const Taskid& id) {
    CHECK(work.find(id) != work.end());
    TaskState *t = work[id];
    CHECK(t->status == TaskState::ACTIVE);
    t->status = TaskState::FINISHED;
  }

#define COUNT_TASKS(name, type)\
  size_t num_ ## name() const {\
    int c = 0;\
    for (TaskMap::const_iterator i = work.begin(); i != work.end(); ++i)\
      if (i->second->status == TaskState::type) { ++c; }\
    return c;\
  }\
  int64_t name ## _size() const {\
      int64_t c = 0;\
      for (TaskMap::const_iterator i = work.begin(); i != work.end(); ++i)\
        if (i->second->status == TaskState::type) { c += i->second->size; }\
      return c;\
  }\
  vector<TaskState*> name() const {\
    vector<TaskState*> out;\
    for (TaskMap::const_iterator i = work.begin(); i != work.end(); ++i)\
      if (i->second->status == TaskState::type) { out.push_back(i->second); }\
    return out;\
  }

  COUNT_TASKS(pending, PENDING)
  COUNT_TASKS(active, ACTIVE)
  COUNT_TASKS(finished, FINISHED)
#undef COUNT_TASKS

  int num_assigned() const {
    return work.size();
  }
  int64_t total_size() const {
    int64_t out = 0;
    for (TaskMap::const_iterator i = work.begin(); i != work.end(); ++i) {
      out += 1 + i->second->size;
    }
    return out;
  }

  // Order pending tasks by our guess of how large they are
  bool get_next(const RunDescriptor& r, KernelRequest* msg) {
    vector<TaskState*> p = pending();

    if (p.empty()) {
      return false;
    }

    TaskState* best = *max_element(p.begin(), p.end(),
        &TaskState::WeightCompare);

    msg->set_kernel(r.kernel);
    msg->set_method(r.method);
    msg->set_table(r.table->id);
    msg->set_shard(best->id.shard);

    best->status = TaskState::ACTIVE;
    last_task_start = Now();

    return true;
  }
};

Master::Master(const ConfigData &conf) :
    tables_(TableRegistry::tables()) {
  config_.CopyFrom(conf);
  kernel_epoch_ = 0;
  finished_ = dispatched_ = 0;

  network_ = rpc::NetworkThread::Get();
  shards_assigned_ = false;

  CHECK_GT(network_->size(), 1)<< "At least one master and one worker required!";

  for (int i = 0; i < config_.num_workers(); ++i) {
    workers_.push_back(new WorkerState(i));
  }

  for (int i = 0; i < config_.num_workers(); ++i) {
    RegisterWorkerRequest req;
    int src = 0;
    network_->Read(rpc::ANY_SOURCE, MTYPE_REGISTER_WORKER, &req, &src);
    VLOG(1) << "Registered worker " << src - 1 << "; "
               << config_.num_workers() - 1 - i << " remaining.";
  }

  LOG(INFO)<< "All workers registered; starting up.";
}

Master::~Master() {
  LOG(INFO)<< "Total runtime: " << runtime_.elapsed();

  LOG(INFO) << "Worker execution time:";
  for (size_t i = 0; i < workers_.size(); ++i) {
    WorkerState& w = *workers_[i];
    if (i % 10 == 0) {
      fprintf(stderr, "\n%zu: ", i);
    }
    fprintf(stderr, "%.3f ", w.total_runtime);
  }
  fprintf(stderr, "\n");

  LOG(INFO) << "Kernel stats: ";
  for (MethodStatsMap::iterator i = method_stats_.begin(); i != method_stats_.end(); ++i) {
    LOG(INFO) << i->first << "--> " << i->second.ShortDebugString();
  }

  LOG(INFO) << "Shutting down workers.";
  EmptyMessage msg;
  for (int i = 1; i < network_->size(); ++i) {
    network_->Send(i, MTYPE_WORKER_SHUTDOWN, msg);
  }
}

WorkerState* Master::worker_for_shard(int table, int shard) {
  for (size_t i = 0; i < workers_.size(); ++i) {
    if (workers_[i]->serves(Taskid(table, shard))) {
      return workers_[i];
    }
  }

  return NULL;
}

WorkerState* Master::assign_worker(int table, int shard) {
  WorkerState* ws = worker_for_shard(table, shard);
  int64_t work_size = 1;

  if (ws) {
//    LOG(INFO) << "Worker for shard: " << MP(table, shard, ws->id);
    ws->assign_task(new TaskState(Taskid(table, shard), work_size));
    return ws;
  }

  WorkerState* best = NULL;
  for (size_t i = 0; i < workers_.size(); ++i) {
    WorkerState& w = *workers_[i];
    if (w.alive() && (best == NULL || w.shards.size() < best->shards.size())) {
      best = workers_[i];
    }
  }

  CHECK(best != NULL)
                         << "Ran out of workers!  Increase the number of partitions per worker!";

//  LOG(INFO) << "Assigned " << MP(table, shard, best->id);
  CHECK(best->alive());

  VLOG(1) << "Assigning " << MP(table, shard) << " to " << best->id;
  best->assign_shard(shard, true);
  best->assign_task(new TaskState(Taskid(table, shard), work_size));
  return best;
}

void Master::send_table_assignments() {
  ShardAssignmentRequest req;

  for (size_t i = 0; i < workers_.size(); ++i) {
    WorkerState& w = *workers_[i];
    for (ShardSet::iterator j = w.shards.begin(); j != w.shards.end(); ++j) {
      ShardAssignment* s = req.add_assign();
      s->set_new_worker(i);
      s->set_table(j->table);
      s->set_shard(j->shard);
//      s->set_old_worker(-1);
    }
  }

  network_->SyncBroadcast(MTYPE_SHARD_ASSIGNMENT, req);
}

bool Master::steal_work(const RunDescriptor& r, int idle_worker,
    double avg_completion_time) {
  if (!FLAGS_work_stealing) {
    return false;
  }

  WorkerState &dst = *workers_[idle_worker];

  if (!dst.alive()) {
    return false;
  }

  // Find the worker with the largest number of pending tasks.
  WorkerState& src = **max_element(workers_.begin(), workers_.end(),
      &WorkerState::PendingCompare);
  if (src.num_pending() == 0) {
    return false;
  }

  vector<TaskState*> pending = src.pending();

  TaskState *task = *max_element(pending.begin(), pending.end(),
      TaskState::WeightCompare);
  if (task->stolen) {
    return false;
  }

  double average_size = 0;

  for (int i = 0; i < r.table->numShards; ++i) {
    average_size += 1;
  }
  average_size /= r.table->numShards;

  // Weight the cost of moving the table versus the time savings.
  double move_cost = std::max(1.0,
      2 * task->size * avg_completion_time / average_size);
  double eta = 0;
  for (size_t i = 0; i < pending.size(); ++i) {
    TaskState *p = pending[i];
    eta += std::max(1.0, p->size * avg_completion_time / average_size);
  }

//  LOG(INFO) << "ETA: " << eta << " move cost: " << move_cost;

  if (eta <= move_cost) {
    return false;
  }

  const Taskid& tid = task->id;
  task->stolen = true;

  LOG(INFO)<< "Worker " << idle_worker << " is stealing task "
  << MP(tid.shard, task->size) << " from worker " << src.id;
  dst.assign_shard(tid.shard, true);
  src.assign_shard(tid.shard, false);

  src.remove_task(task);
  dst.assign_task(task);
  return true;
}

void Master::assign_tables() {
  shards_assigned_ = true;

  // Assign workers for all table shards, to ensure every shard has an workerForShard.
  TableRegistry::Map &tables = TableRegistry::tables();
  for (TableRegistry::Map::iterator i = tables.begin(); i != tables.end();
      ++i) {
    if (!i->second->numShards) {
      VLOG(2) << "Note: assigning tables; table " << i->first
                 << " has no shards.";
    }
    for (int j = 0; j < i->second->numShards; ++j) {
      assign_worker(i->first, j);
    }
  }
}

void Master::assign_tasks(const RunDescriptor& r, vector<int> shards) {
  for (size_t i = 0; i < workers_.size(); ++i) {
    WorkerState& w = *workers_[i];
    w.clear_tasks(); //XXX: did not delete task state, memory leak
  }

  for (size_t i = 0; i < shards.size(); ++i) {
    VLOG(1) << "Assigning worker for table " << r.table->id << " for shard "
               << i << " of " << shards.size();
    assign_worker(r.table->id, shards[i]);
  }
}

int Master::dispatch_work(const RunDescriptor& r) {
  int num_dispatched = 0;
  KernelRequest w_req;
  for (size_t i = 0; i < workers_.size(); ++i) {
    WorkerState& w = *workers_[i];
    if (w.num_pending() > 0 && w.num_active() == 0) {
      w.get_next(r, &w_req);
      num_dispatched++;
      network_->Send(w.id + 1, MTYPE_RUN_KERNEL, w_req);
    }
  }
  return num_dispatched;
}

void Master::dump_stats() {
  string status;
  for (int k = 0; k < config_.num_workers(); ++k) {
    status += StringPrintf("%d/%d ", workers_[k]->num_finished(),
        workers_[k]->num_assigned());
  }
  LOG(INFO)<< StringPrintf("Running %s (%d); %s; assigned: %d done: %d",
      current_run_.method.c_str(), current_run_.shards.size(),
      status.c_str(), dispatched_, finished_);

}

int Master::reap_one_task() {
  MethodStats &mstats = method_stats_[current_run_.kernel + ":"
      + current_run_.method];
  KernelDone done_msg;
  int w_id = 0;

  if (network_->TryRead(rpc::ANY_SOURCE, MTYPE_KERNEL_DONE, &done_msg, &w_id)) {

    w_id -= 1;

    WorkerState& w = *workers_[w_id];

    Taskid task_id(done_msg.kernel().table(), done_msg.kernel().shard());

    for (int i = 0; i < done_msg.shards_size(); ++i) {
      const ShardInfo &si = done_msg.shards(i);
      tables_[si.table()]->updatePartitions(si);
    }

    w.set_finished(task_id);

    w.total_runtime += Now() - w.last_task_start;
    mstats.set_shard_time(mstats.shard_time() + Now() - w.last_task_start);
    mstats.set_shard_calls(mstats.shard_calls() + 1);
    w.ping();
    return w_id;
  } else {
    Sleep(FLAGS_sleep_time);
    return -1;
  }

}

void Master::run(RunDescriptor r) {
  // HACKHACKHACK - register ourselves with any existing tables
  for (TableRegistry::Map::iterator i = tables_.begin(); i != tables_.end();
      ++i) {
    i->second->setHelper((TableHelper*)this);
  }

  CHECK_EQ(current_run_.shards.size(), finished_)<< " Cannot start kernel before previous one is finished ";
  finished_ = dispatched_ = 0;

  KernelInfo *k = KernelRegistry::Get()->kernel(r.kernel);
  CHECK_NE(r.table, (void*)NULL)<< "Table locality must be specified!";
  CHECK_NE(k, (void*)NULL)<< "Invalid kernel class " << r.kernel;
  CHECK_EQ(k->has_method(r.method), true)<< "Invalid method: " << MP(r.kernel, r.method);

  VLOG(1) << "Running: " << r.kernel << " : " << r.method << " on table "
             << r.table->id;

  vector<int> shards = r.shards;

  MethodStats &mstats = method_stats_[r.kernel + ":" + r.method];
  mstats.set_calls(mstats.calls() + 1);

  current_run_ = r;
  current_run_start_ = Now();

  if (!shards_assigned_) {
    //only perform table assignment before the first kernel run
    assign_tables();
    send_table_assignments();
  }

  kernel_epoch_++;

  VLOG(1) << "Current run: " << shards.size() << " shards";
  assign_tasks(current_run_, shards);

  dispatched_ = dispatch_work(current_run_);
  barrier();
}

void Master::barrier() {
  MethodStats &mstats = method_stats_[current_run_.kernel + ":"
      + current_run_.method];

  VLOG(3) << "Starting barrier() with finished_=" << finished_;

  while (finished_ < current_run_.shards.size()) {
    PERIODIC(10, {DumpProfile(); dump_stats();});

    double lastWorkCheck = Now();
    if (reap_one_task() >= 0) {
      finished_++;
      if (lastWorkCheck - Now() > 0.1) {
        lastWorkCheck = Now();
        double avg_completion_time = mstats.shard_time() / mstats.shard_calls();

        bool need_update = false;
        for (int i = 0; i < workers_.size(); ++i) {
          WorkerState& w = *workers_[i];

          // Don't try to steal tasks if the payoff is too small.
          if (mstats.shard_calls() > 10 && avg_completion_time > 0.2
              && w.idle_time() > 0.5) {
            if (steal_work(current_run_, w.id, avg_completion_time)) {
              need_update = true;
            }
          }

          if (need_update) {
            // Update the table assignments.
            send_table_assignments();
          }

        }
      }
      if (dispatched_ < current_run_.shards.size()) {
        dispatched_ += dispatch_work(current_run_);
      }
    }

    VLOG(3) << "All kernels finished in barrier() with finished_=" << finished_;
    VLOG(1) << "Kernels finished, in flush/apply phase";

    bool quiescent;
    EmptyMessage empty;
    int worker_id = 0;
    do {
      quiescent = true;

      //1st round-trip to make sure all workers have flushed everything
      network_->Broadcast(MTYPE_WORKER_FLUSH, empty);
      VLOG(2) << "Sent flush broadcast to workers" << endl;

      size_t flushed = 0;
      size_t applied = 0;
      FlushResponse done_msg;
      while (flushed < workers_.size()) {
        VLOG(3) << "Waiting for flush responses (" << flushed << " received)"
                   << endl;
        if (network_->TryRead(rpc::ANY_SOURCE, MTYPE_FLUSH_RESPONSE, &done_msg,
            &worker_id)) {
          flushed++;
          if (done_msg.updatesdone() > 0) {
            quiescent = false;
          }

          VLOG(1) << "Received flush response " << flushed << " of "
                     << workers_.size() << " with " << done_msg.updatesdone()
                     << " updates done.";
        } else {
          Sleep(FLAGS_sleep_time);
        }
      }
    } while (1);

    //2nd round-trip to make sure all workers have applied all updates
    network_->Broadcast(MTYPE_WORKER_APPLY, empty);
    VLOG(2) << "Sent apply broadcast to workers" << endl;

    mstats.set_total_time(mstats.total_time() + Now() - current_run_start_);
    LOG(INFO)<< "Kernel '" << current_run_.method << "' finished in " << Now() - current_run_start_;
  }
}

} // namespace piccolo
