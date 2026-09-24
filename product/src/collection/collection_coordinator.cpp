#include "collection/collection_coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

namespace pdcm {
namespace {

struct ProviderItemLess {
  bool operator()(const ProviderReadItem &lhs,
                  const ProviderReadItem &rhs) const noexcept {
    return std::tie(lhs.entity.kind, lhs.entity.id.value, lhs.entity.generation,
                    lhs.kind, lhs.data_id) <
           std::tie(rhs.entity.kind, rhs.entity.id.value, rhs.entity.generation,
                    rhs.kind, rhs.data_id);
  }
};

struct GroupKey {
  std::string provider_id;
  Nanoseconds period{0};
  ProviderDataKind kind{ProviderDataKind::kMetric};
  std::uint32_t isolation_class{0};

  friend bool operator<(const GroupKey &lhs, const GroupKey &rhs) noexcept {
    return std::tie(lhs.provider_id, lhs.period, lhs.kind,
                    lhs.isolation_class) <
           std::tie(rhs.provider_id, rhs.period, rhs.kind, rhs.isolation_class);
  }
};

pdcm_status_t statusFor(const ObservationStatus status) {
  switch (status) {
  case ObservationStatus::kNotAvailable:
    return PDCM_STATUS_UNAVAILABLE;
  case ObservationStatus::kUnsupported:
    return PDCM_STATUS_UNSUPPORTED;
  case ObservationStatus::kError:
  case ObservationStatus::kStale:
    return PDCM_STATUS_INTERNAL;
  case ObservationStatus::kValid:
    return PDCM_STATUS_SUCCESS;
  }
  return PDCM_STATUS_INTERNAL;
}

ObservationStatus callFailureStatus(const pdcm_status_t status) {
  if (status == PDCM_STATUS_UNAVAILABLE ||
      status == PDCM_STATUS_NOT_INITIALIZED) {
    return ObservationStatus::kNotAvailable;
  }
  if (status == PDCM_STATUS_UNSUPPORTED) {
    return ObservationStatus::kUnsupported;
  }
  return ObservationStatus::kError;
}

bool usableCallStatus(const pdcm_status_t status) {
  return status == PDCM_STATUS_SUCCESS || status == PDCM_STATUS_PARTIAL_RESULT;
}

bool validProviderItem(const ProviderReadItemResult &item) {
  if (item.status == ObservationStatus::kStale ||
      (item.source_sample_time_ns.has_value() &&
       *item.source_sample_time_ns < 0)) {
    return false;
  }
  return item.status == ObservationStatus::kValid ? item.value.has_value()
                                                  : !item.value.has_value();
}

bool valueMatches(const MetricDescriptor &descriptor,
                  const MetricValue &value) {
  switch (descriptor.value_type) {
  case MetricValueKind::kInt64:
    return std::holds_alternative<std::int64_t>(value);
  case MetricValueKind::kUint64:
  case MetricValueKind::kEnum:
    return std::holds_alternative<std::uint64_t>(value);
  case MetricValueKind::kDouble:
    return std::holds_alternative<double>(value);
  case MetricValueKind::kBool:
    return std::holds_alternative<bool>(value);
  case MetricValueKind::kString:
    return std::holds_alternative<std::string>(value);
  }
  return false;
}

std::size_t resultItemBytes(const ProviderReadItemResult &item) {
  const std::size_t value_bytes =
      item.value.has_value() && std::holds_alternative<std::string>(*item.value)
          ? std::get<std::string>(*item.value).size()
          : 0;
  return sizeof(ProviderReadItemResult) + item.native_source.size() +
         value_bytes;
}

} // namespace

struct CollectionCoordinator::Runtime {
  struct Completion {
    std::mutex mutex;
    std::condition_variable changed;
    bool done{false};
    Status status;
  };

  struct FreshSignature {
    std::uint64_t catalog_generation{0};
    std::vector<DataKey> keys;

    friend bool operator<(const FreshSignature &lhs,
                          const FreshSignature &rhs) noexcept {
      if (lhs.catalog_generation != rhs.catalog_generation) {
        return lhs.catalog_generation < rhs.catalog_generation;
      }
      return lhs.keys < rhs.keys;
    }
  };

  struct Task {
    std::shared_ptr<const CollectionPlan> plan;
    CollectionJob job;
    ProviderReadRequest request;
    std::vector<DataKey> keys;
    WatchPriority priority{WatchPriority::kNormal};
    std::optional<FreshSignature> fresh_signature;
    std::shared_ptr<Completion> completion;
  };

  enum class EnqueueResult : std::uint8_t {
    kAccepted,
    kBusy,
    kFull,
    kStopped,
  };

  explicit Runtime(CollectionCoordinator &coordinator) : owner(coordinator) {}

  void startWorker() {
    worker = std::thread([this] { workerLoop(); });
  }

  void setCompletion(const std::shared_ptr<Task> &task, Status status) {
    if (task->completion == nullptr) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(task->completion->mutex);
      task->completion->status = std::move(status);
      task->completion->done = true;
    }
    task->completion->changed.notify_all();
  }

  void releaseTaskLocked(const std::shared_ptr<Task> &task) {
    for (const DataKey &key : task->keys) {
      busy_keys.erase(key);
    }
    if (task->fresh_signature.has_value()) {
      const auto found = fresh_tasks.find(*task->fresh_signature);
      if (found != fresh_tasks.end()) {
        const std::shared_ptr<Task> registered = found->second.lock();
        if (registered == nullptr || registered == task) {
          fresh_tasks.erase(found);
        }
      }
    }
  }

  EnqueueResult enqueueLocked(const std::shared_ptr<Task> &task) {
    if (shutdown) {
      return EnqueueResult::kStopped;
    }
    for (const DataKey &key : task->keys) {
      if (busy_keys.find(key) != busy_keys.end()) {
        return EnqueueResult::kBusy;
      }
    }

    if (queue.size() == owner.limits_.max_provider_queue) {
      if (queue.empty() || task->priority <= queue.back()->priority) {
        return EnqueueResult::kFull;
      }
      const std::shared_ptr<Task> evicted = queue.back();
      queue.pop_back();
      releaseTaskLocked(evicted);
      setCompletion(evicted,
                    Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                           "provider queue evicted a lower-priority request"));
    }

    for (const DataKey &key : task->keys) {
      busy_keys.insert(key);
    }
    if (task->fresh_signature.has_value()) {
      fresh_tasks[*task->fresh_signature] = task;
    }
    const auto position =
        std::find_if(queue.begin(), queue.end(),
                     [&task](const std::shared_ptr<Task> &queued) {
                       return queued->priority < task->priority;
                     });
    queue.insert(position, task);
    work_changed.notify_one();
    return EnqueueResult::kAccepted;
  }

  void installPlan(const std::shared_ptr<const CollectionPlan> &next) {
    std::lock_guard<std::mutex> lock(mutex);
    scheduled_plan_version = next->version;
    next_due.clear();
    for (const CollectionJob &job : next->jobs) {
      next_due.emplace(job.id, next->created_at + job.period);
    }
    scheduler_changed.notify_all();
  }

  Status activateCatalog(std::shared_ptr<const CatalogView> catalog) {
    if (catalog == nullptr || catalog->generation() == 0 ||
        catalog->generation() != owner.data_.catalogGeneration()) {
      return Status(PDCM_STATUS_STALE_GENERATION,
                    "collection route catalog is stale");
    }

    std::lock_guard<std::mutex> lock(mutex);
    route_generation = catalog->generation();
    route_provider_id.clear();
    route_entity.reset();
    route_metrics.clear();
    if (catalog->topologyUnsupported() || catalog->entities().size() != 1) {
      return Status(PDCM_STATUS_UNSUPPORTED,
                    "collection routing requires one active device");
    }

    const EntityRecord &entity = catalog->entities().front();
    const CapabilityQueryResult capabilities =
        catalog->capabilities(entity.ref);
    if (!capabilities.status.ok() || !capabilities.capabilities.has_value() ||
        entity.provider_version.empty()) {
      return Status(PDCM_STATUS_UNAVAILABLE,
                    "collection routing capabilities are unavailable");
    }
    route_provider_id = entity.provider_version;
    route_entity = entity.ref;
    for (const CapabilityItem &item : capabilities.capabilities->items) {
      if (item.kind == CapabilityKind::kMetric && item.supported) {
        route_metrics.insert(item.id);
      }
    }
    return Status::success();
  }

  SchedulerRunSummary runDue(const MonotonicTime now) {
    SchedulerRunSummary summary;
    if (now.time_since_epoch().count() < 0) {
      summary.status =
          Status(PDCM_STATUS_INVALID_ARGUMENT, "scheduler time is invalid");
      return summary;
    }

    struct LogicalResult {
      CollectionJob job;
      MonotonicTime first_scheduled_time;
      std::uint64_t sample_count{0};
    };
    std::vector<LogicalResult> logical_results;
    const std::shared_ptr<const CollectionPlan> current = owner.plan();
    std::unique_lock<std::mutex> lock(mutex);
    if (shutdown) {
      summary.status =
          Status(PDCM_STATUS_NOT_INITIALIZED, "collection runtime is stopped");
      return summary;
    }
    if (scheduled_plan_version != current->version) {
      scheduled_plan_version = current->version;
      next_due.clear();
      for (const CollectionJob &job : current->jobs) {
        next_due.emplace(job.id, current->created_at + job.period);
      }
    }

    for (const CollectionJob &job : current->jobs) {
      auto due_entry = next_due.find(job.id);
      if (due_entry == next_due.end() || now < due_entry->second) {
        continue;
      }
      const std::int64_t elapsed = (now - due_entry->second).count();
      const std::int64_t steps = elapsed / job.period.count() + 1;
      summary.missed_periods += static_cast<std::size_t>(steps - 1);
      const MonotonicTime first_due = due_entry->second;
      const MonotonicTime scheduled =
          due_entry->second + job.period * (steps - 1);
      due_entry->second += job.period * steps;

      Nanoseconds deadline_window = job.items.front().freshness;
      auto task = std::make_shared<Task>();
      task->plan = current;
      task->job = job;
      task->priority = job.priority;
      task->request.request_id = owner.allocateRequestId();
      task->request.plan_generation = current->version;
      task->request.catalog_generation = current->catalog_generation;
      task->request.scheduled_time = scheduled;
      for (const CollectionPlanItem &item : job.items) {
        deadline_window = std::min(deadline_window, item.freshness);
        task->request.items.push_back(
            ProviderReadItem{item.key.entity, item.key.kind, item.key.data_id});
        task->keys.push_back(
            DataKey{item.key.entity, MetricId{item.key.data_id}});
      }
      task->request.deadline = scheduled + deadline_window;
      if (task->request.request_id == 0) {
        ++summary.queue_rejections;
        logical_results.push_back(
            {job, first_due, static_cast<std::uint64_t>(steps)});
        continue;
      }

      const EnqueueResult queued = enqueueLocked(task);
      switch (queued) {
      case EnqueueResult::kAccepted:
        ++summary.enqueued_jobs;
        break;
      case EnqueueResult::kBusy:
        ++summary.busy_jobs;
        break;
      case EnqueueResult::kFull:
      case EnqueueResult::kStopped:
        ++summary.queue_rejections;
        break;
      }
      if (queued == EnqueueResult::kAccepted && steps > 1) {
        logical_results.push_back(
            {job, first_due, static_cast<std::uint64_t>(steps - 1)});
      } else if (queued != EnqueueResult::kAccepted) {
        logical_results.push_back(
            {job, first_due, static_cast<std::uint64_t>(steps)});
      }
    }

    lock.unlock();
    for (const LogicalResult &result : logical_results) {
      owner.noteLogicalResults(result.job, result.first_scheduled_time,
                               result.job.period, result.sample_count);
    }

    summary.status = summary.busy_jobs == 0 && summary.queue_rejections == 0
                         ? Status::success()
                         : Status(PDCM_STATUS_PARTIAL_RESULT,
                                  "one or more due jobs could not be queued");
    return summary;
  }

  Status freshRead(std::vector<DataKey> keys,
                   const std::uint64_t catalog_generation,
                   const MonotonicTime deadline) {
    if (keys.empty() || catalog_generation == 0) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "fresh read arguments are invalid");
    }
    if (keys.size() > owner.limits_.max_batch_items) {
      return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                    "fresh read batch item limit exceeded");
    }
    if (deadline <= owner.clock_->monotonicNow()) {
      return Status(PDCM_STATUS_TIMEOUT, "fresh read deadline expired");
    }
    std::sort(keys.begin(), keys.end());
    if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "fresh read contains duplicate keys");
    }

    std::shared_ptr<Completion> completion;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (shutdown) {
        return Status(PDCM_STATUS_NOT_INITIALIZED,
                      "collection runtime is stopped");
      }
      if (catalog_generation != route_generation || !route_entity.has_value()) {
        return Status(PDCM_STATUS_STALE_GENERATION,
                      "fresh read route is stale");
      }
      for (const DataKey &key : keys) {
        if (key.entity != *route_entity ||
            route_metrics.find(key.metric.value) == route_metrics.end() ||
            !owner.data_.metricDescriptor(key.metric).has_value()) {
          return Status(PDCM_STATUS_UNSUPPORTED,
                        "fresh read key is not supported");
        }
      }
      if (fresh_waiters == owner.limits_.max_fresh_waiters) {
        return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                      "fresh read waiter limit exceeded");
      }

      FreshSignature signature{catalog_generation, keys};
      const auto existing = fresh_tasks.find(signature);
      if (existing != fresh_tasks.end()) {
        const std::shared_ptr<Task> task = existing->second.lock();
        if (task != nullptr && task->request.deadline >= deadline) {
          completion = task->completion;
        } else if (task != nullptr) {
          return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                        "in-flight fresh read has an incompatible deadline");
        } else {
          fresh_tasks.erase(existing);
        }
      }

      if (completion == nullptr) {
        std::shared_ptr<const CollectionPlan> current = owner.plan();
        if (current->catalog_generation != catalog_generation) {
          auto standalone = std::make_shared<CollectionPlan>(*current);
          standalone->catalog_generation = catalog_generation;
          current = std::static_pointer_cast<const CollectionPlan>(standalone);
        }
        auto task = std::make_shared<Task>();
        task->plan = current;
        task->job.provider_id = route_provider_id;
        task->job.kind = ProviderDataKind::kMetric;
        task->job.priority = WatchPriority::kNormal;
        task->priority = WatchPriority::kNormal;
        task->keys = keys;
        task->fresh_signature = signature;
        task->completion = std::make_shared<Completion>();
        task->request.request_id = owner.allocateRequestId();
        task->request.plan_generation = current->version;
        task->request.catalog_generation = catalog_generation;
        task->request.scheduled_time = owner.clock_->monotonicNow();
        task->request.deadline = deadline;
        if (task->request.request_id == 0) {
          return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                        "request ID space exhausted");
        }

        for (const DataKey &key : keys) {
          const std::optional<MetricDescriptor> descriptor =
              owner.data_.metricDescriptor(key.metric);
          CollectionPlanItem item;
          item.key.provider_id = route_provider_id;
          item.key.entity = key.entity;
          item.key.kind = ProviderDataKind::kMetric;
          item.key.data_id = key.metric.value;
          item.freshness =
              Nanoseconds{static_cast<std::int64_t>(descriptor->freshness_ns)};
          item.priority = WatchPriority::kNormal;
          task->job.items.push_back(item);
          task->request.items.push_back(ProviderReadItem{
              key.entity, ProviderDataKind::kMetric, key.metric.value});
        }

        const EnqueueResult queued = enqueueLocked(task);
        if (queued == EnqueueResult::kBusy) {
          return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                        "fresh read key is already in flight");
        }
        if (queued != EnqueueResult::kAccepted) {
          return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                        "provider queue is full");
        }
        completion = task->completion;
      }
      ++fresh_waiters;
    }

    Status status;
    {
      std::unique_lock<std::mutex> lock(completion->mutex);
      if (!completion->changed.wait_until(
              lock, deadline, [&completion] { return completion->done; })) {
        status = Status(PDCM_STATUS_TIMEOUT, "fresh read waiter timed out");
      } else {
        status = completion->status;
      }
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      --fresh_waiters;
    }
    return status;
  }

  Status startScheduler() {
    std::lock_guard<std::mutex> lock(mutex);
    if (shutdown) {
      return Status(PDCM_STATUS_NOT_INITIALIZED,
                    "collection runtime is stopped");
    }
    if (scheduler_running) {
      return Status::success();
    }
    scheduler_stop = false;
    scheduler_running = true;
    scheduler = std::thread([this] { schedulerLoop(); });
    return Status::success();
  }

  void stopScheduler() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!scheduler_running) {
        return;
      }
      scheduler_stop = true;
      scheduler_changed.notify_all();
    }
    if (scheduler.joinable()) {
      scheduler.join();
    }
    std::lock_guard<std::mutex> lock(mutex);
    scheduler_running = false;
  }

  bool schedulerRunning() const noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    return scheduler_running;
  }

  void shutdownAll() noexcept {
    stopScheduler();
    std::vector<std::shared_ptr<Task>> cancelled;
    {
      std::lock_guard<std::mutex> lock(mutex);
      shutdown = true;
      while (!queue.empty()) {
        cancelled.push_back(queue.front());
        releaseTaskLocked(queue.front());
        queue.pop_front();
      }
      work_changed.notify_all();
    }
    for (const std::shared_ptr<Task> &task : cancelled) {
      setCompletion(task, Status(PDCM_STATUS_NOT_INITIALIZED,
                                 "collection runtime stopped"));
    }
    if (worker.joinable()) {
      worker.join();
    }
  }

  void workerLoop() {
    for (;;) {
      std::shared_ptr<Task> task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        work_changed.wait(lock, [this] { return shutdown || !queue.empty(); });
        if (shutdown && queue.empty()) {
          return;
        }
        task = queue.front();
        queue.pop_front();
        ++provider_in_flight;
      }

      ProviderReadResult provider_result;
      if (owner.clock_->monotonicNow() > task->request.deadline) {
        provider_result.call_status =
            Status(PDCM_STATUS_TIMEOUT,
                   "collection task expired in the provider queue");
      } else {
        provider_result = owner.provider_.batchRead(task->request);
      }
      const CollectionRunResult run = owner.normalizeAndCommit(
          *task->plan, task->job, task->request, provider_result);

      if (task->job.id != 0) {
        owner.noteLogicalResults(task->job, task->request.scheduled_time,
                                 task->job.period, 1);
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        --provider_in_flight;
        releaseTaskLocked(task);
      }
      setCompletion(task, run.status);
    }
  }

  void schedulerLoop() {
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (scheduler_stop || shutdown) {
          return;
        }
      }
      (void)owner.runDueOnce(owner.clock_->monotonicNow());
      std::unique_lock<std::mutex> lock(mutex);
      scheduler_changed.wait_for(lock, owner.limits_.scheduler_max_sleep,
                                 [this] { return scheduler_stop || shutdown; });
      if (scheduler_stop || shutdown) {
        return;
      }
    }
  }

  CollectionCoordinator &owner;
  mutable std::mutex mutex;
  std::condition_variable work_changed;
  std::condition_variable scheduler_changed;
  std::deque<std::shared_ptr<Task>> queue;
  std::set<DataKey> busy_keys;
  std::map<FreshSignature, std::weak_ptr<Task>> fresh_tasks;
  std::map<std::uint64_t, MonotonicTime> next_due;
  std::thread worker;
  std::thread scheduler;
  std::size_t provider_in_flight{0};
  std::size_t fresh_waiters{0};
  std::uint64_t scheduled_plan_version{0};
  std::uint64_t route_generation{0};
  std::string route_provider_id;
  std::optional<EntityRef> route_entity;
  std::set<std::uint32_t> route_metrics;
  bool shutdown{false};
  bool scheduler_stop{false};
  bool scheduler_running{false};
};
Status CollectionLimits::validate() const {
  if (max_plan_jobs == 0 || max_batch_items == 0 || max_result_items == 0 ||
      max_result_bytes == 0 || max_result_items < max_batch_items ||
      max_provider_queue == 0 || max_provider_in_flight != 1 ||
      max_fresh_waiters == 0 || scheduler_max_sleep.count() <= 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "collection limits must be positive and consistent");
  }
  return Status::success();
}

CollectionCoordinator::CollectionCoordinator(ProviderManager &provider,
                                             DataManager &data,
                                             std::shared_ptr<const Clock> clock,
                                             CollectionLimits limits,
                                             WatchManager *watch_manager)
    : provider_(provider), data_(data), clock_(std::move(clock)),
      limits_(limits), runtime_(std::make_unique<Runtime>(*this)),
      watch_manager_(watch_manager) {
  if (clock_ == nullptr) {
    throw std::invalid_argument("CollectionCoordinator requires a clock");
  }
  const Status status = limits_.validate();
  if (!status.ok()) {
    throw std::invalid_argument(status.message());
  }
  plan_ = std::make_shared<CollectionPlan>();
  runtime_->startWorker();
}
CollectionCoordinator::~CollectionCoordinator() { runtime_->shutdownAll(); }

Status CollectionCoordinator::activateCatalog(
    std::shared_ptr<const CatalogView> catalog) {
  return runtime_->activateCatalog(std::move(catalog));
}

Status CollectionCoordinator::applyWatchSnapshot(
    std::shared_ptr<const WatchSnapshot> watches) {
  if (watches == nullptr || watches->catalog_generation == 0 ||
      watches->catalog_generation != data_.catalogGeneration()) {
    return Status(PDCM_STATUS_STALE_GENERATION,
                  "watch snapshot does not match the active data catalog");
  }

  std::map<GroupKey, std::vector<CollectionPlanItem>> groups;
  std::set<EffectiveWatchKey> unique;
  for (const EffectiveWatch &watch : watches->effective_watches) {
    if (watch.key.provider_id.empty() ||
        watch.key.kind != ProviderDataKind::kMetric || watch.key.data_id == 0 ||
        watch.period.count() <= 0 || watch.freshness < watch.period ||
        !unique.insert(watch.key).second ||
        !data_.metricDescriptor(MetricId{watch.key.data_id}).has_value()) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "effective watch cannot be compiled");
    }
    GroupKey group{watch.key.provider_id, watch.period, watch.key.kind,
                   watch.key.isolation_class};
    groups[group].push_back(CollectionPlanItem{watch.key, watch.period,
                                               watch.freshness, watch.priority,
                                               watch.logical_watches});
  }

  auto next = std::make_shared<CollectionPlan>();
  next->watch_snapshot_version = watches->version;
  next->catalog_generation = watches->catalog_generation;
  next->created_at = clock_->monotonicNow();

  std::uint64_t next_job_id = 1;
  for (auto &group : groups) {
    std::vector<CollectionPlanItem> &items = group.second;
    std::sort(items.begin(), items.end(),
              [](const CollectionPlanItem &lhs, const CollectionPlanItem &rhs) {
                return lhs.key < rhs.key;
              });
    for (std::size_t offset = 0; offset < items.size();
         offset += limits_.max_batch_items) {
      if (next->jobs.size() == limits_.max_plan_jobs ||
          next_job_id == std::numeric_limits<std::uint64_t>::max()) {
        return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                      "collection plan job limit exceeded");
      }
      const std::size_t end =
          std::min(items.size(), offset + limits_.max_batch_items);
      CollectionJob job;
      job.id = next_job_id++;
      job.provider_id = group.first.provider_id;
      job.kind = group.first.kind;
      job.isolation_class = group.first.isolation_class;
      job.period = group.first.period;
      job.items.assign(items.begin() + static_cast<std::ptrdiff_t>(offset),
                       items.begin() + static_cast<std::ptrdiff_t>(end));
      for (const CollectionPlanItem &item : job.items) {
        job.priority = std::max(job.priority, item.priority);
      }
      next->jobs.push_back(std::move(job));
    }
  }

  std::lock_guard<std::mutex> lock(plan_writer_mutex_);
  const std::shared_ptr<const CollectionPlan> current =
      std::atomic_load_explicit(&plan_, std::memory_order_acquire);
  if (current->version == std::numeric_limits<std::uint64_t>::max()) {
    return Status(PDCM_STATUS_INTERNAL, "collection plan version overflow");
  }
  next->version = current->version + 1;
  const std::shared_ptr<const CollectionPlan> published =
      std::static_pointer_cast<const CollectionPlan>(next);
  std::atomic_store_explicit(&plan_, published, std::memory_order_release);
  runtime_->installPlan(published);
  return Status::success();
}

std::shared_ptr<const CollectionPlan>
CollectionCoordinator::plan() const noexcept {
  return std::atomic_load_explicit(&plan_, std::memory_order_acquire);
}

CollectionRunResult
CollectionCoordinator::collectJob(const std::uint64_t job_id,
                                  const MonotonicTime scheduled_time,
                                  const MonotonicTime deadline) {
  CollectionRunResult result;
  const std::shared_ptr<const CollectionPlan> current = plan();
  result.plan_version = current->version;
  const auto found = std::find_if(
      current->jobs.begin(), current->jobs.end(),
      [job_id](const CollectionJob &job) { return job.id == job_id; });
  if (found == current->jobs.end()) {
    result.status =
        Status(PDCM_STATUS_NOT_FOUND, "collection job does not exist");
    return result;
  }
  if (scheduled_time.time_since_epoch().count() < 0 ||
      deadline < scheduled_time || deadline < clock_->monotonicNow()) {
    result.status =
        Status(PDCM_STATUS_TIMEOUT, "collection job deadline expired");
    return result;
  }

  const std::uint64_t request_id = allocateRequestId();
  if (request_id == 0) {
    result.status =
        Status(PDCM_STATUS_RESOURCE_EXHAUSTED, "request ID space exhausted");
    return result;
  }

  ProviderReadRequest request;
  request.request_id = request_id;
  request.plan_generation = current->version;
  request.catalog_generation = current->catalog_generation;
  request.scheduled_time = scheduled_time;
  request.deadline = deadline;
  request.items.reserve(found->items.size());
  for (const CollectionPlanItem &item : found->items) {
    request.items.push_back(
        ProviderReadItem{item.key.entity, item.key.kind, item.key.data_id});
  }

  const ProviderReadResult provider_result = provider_.batchRead(request);
  CollectionRunResult run =
      normalizeAndCommit(*current, *found, request, provider_result);
  noteLogicalResults(*found, scheduled_time, found->period, 1);
  return run;
}

std::uint64_t CollectionCoordinator::allocateRequestId() noexcept {
  std::uint64_t request_id = next_request_id_.load(std::memory_order_relaxed);
  while (request_id != 0 &&
         request_id != std::numeric_limits<std::uint64_t>::max() &&
         !next_request_id_.compare_exchange_weak(request_id, request_id + 1,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
  }
  return request_id == std::numeric_limits<std::uint64_t>::max() ? 0
                                                                 : request_id;
}

void CollectionCoordinator::noteLogicalResults(
    const CollectionJob &job, const MonotonicTime first_scheduled_time,
    const Nanoseconds period, const std::uint64_t sample_count) {
  if (watch_manager_ == nullptr) {
    return;
  }
  std::vector<WatchId> watch_ids;
  for (const CollectionPlanItem &item : job.items) {
    watch_ids.insert(watch_ids.end(), item.logical_watches.begin(),
                     item.logical_watches.end());
  }
  std::sort(watch_ids.begin(), watch_ids.end());
  watch_ids.erase(std::unique(watch_ids.begin(), watch_ids.end()),
                  watch_ids.end());
  if (watch_ids.empty()) {
    return;
  }

  const Status counted = watch_manager_->recordSamples(
      std::move(watch_ids), first_scheduled_time, period, sample_count);
  if (!counted.ok()) {
    return;
  }
  const std::shared_ptr<const WatchSnapshot> next = watch_manager_->snapshot();
  if (next->version != plan()->watch_snapshot_version) {
    (void)applyWatchSnapshot(next);
  }
}

SchedulerRunSummary CollectionCoordinator::runDueOnce(const MonotonicTime now) {
  return runtime_->runDue(now);
}

Status CollectionCoordinator::startScheduler() {
  return runtime_->startScheduler();
}

void CollectionCoordinator::stopScheduler() noexcept {
  runtime_->stopScheduler();
}

bool CollectionCoordinator::schedulerRunning() const noexcept {
  return runtime_->schedulerRunning();
}

Status CollectionCoordinator::freshRead(const std::vector<DataKey> &keys,
                                        const std::uint64_t catalog_generation,
                                        const MonotonicTime deadline) {
  return runtime_->freshRead(keys, catalog_generation, deadline);
}
CollectionRunResult CollectionCoordinator::normalizeAndCommit(
    const CollectionPlan &plan, const CollectionJob &job,
    const ProviderReadRequest &request,
    const ProviderReadResult &provider_result) {
  CollectionRunResult result;
  result.request_id = request.request_id;
  result.plan_version = plan.version;
  result.requested_items = request.items.size();

  if (provider_result.items.size() > limits_.max_result_items) {
    result.status = Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                           "provider result item limit exceeded");
    return result;
  }
  std::size_t result_bytes = 0;
  for (const ProviderReadItemResult &item : provider_result.items) {
    const std::size_t item_bytes = resultItemBytes(item);
    if (item_bytes > limits_.max_result_bytes - result_bytes) {
      result.status = Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                             "provider result byte limit exceeded");
      return result;
    }
    result_bytes += item_bytes;
  }
  if (data_.catalogGeneration() != plan.catalog_generation) {
    result.status = Status(PDCM_STATUS_STALE_GENERATION,
                           "collection result belongs to an old catalog");
    return result;
  }

  std::map<ProviderReadItem, std::size_t, ProviderItemLess> requested;
  for (std::size_t index = 0; index < request.items.size(); ++index) {
    requested.emplace(request.items[index], index);
  }

  std::map<ProviderReadItem, ProviderReadItemResult, ProviderItemLess> returned;
  std::set<ProviderReadItem, ProviderItemLess> duplicated;
  if (usableCallStatus(provider_result.call_status.code())) {
    for (const ProviderReadItemResult &item : provider_result.items) {
      if (requested.find(item.item) == requested.end()) {
        ++result.contract_violations;
        continue;
      }
      if (!returned.emplace(item.item, item).second) {
        duplicated.insert(item.item);
        ++result.contract_violations;
      }
    }
  }

  const std::int64_t observed_time =
      clock_->monotonicNow().time_since_epoch().count();
  const std::int64_t wall_time = clock_->wallTimeNanoseconds();
  std::vector<Observation> observations;
  observations.reserve(request.items.size());

  for (const ProviderReadItem &requested_item : request.items) {
    ProviderReadItemResult item;
    Status error = Status::success();

    if (!usableCallStatus(provider_result.call_status.code())) {
      item.item = requested_item;
      item.status = callFailureStatus(provider_result.call_status.code());
      error = provider_result.call_status;
    } else {
      const auto found = returned.find(requested_item);
      if (duplicated.find(requested_item) != duplicated.end()) {
        continue;
      }
      if (found == returned.end()) {
        item.item = requested_item;
        item.status = ObservationStatus::kError;
        error = Status(PDCM_STATUS_INTERNAL, "provider omitted requested item");
      } else {
        item = found->second;
        if (!validProviderItem(item)) {
          ++result.contract_violations;
          continue;
        }
        if (item.status != ObservationStatus::kValid) {
          error = Status(statusFor(item.status), "provider item read failed");
        }
      }
    }

    const std::optional<MetricDescriptor> descriptor =
        data_.metricDescriptor(MetricId{requested_item.data_id});
    if (!descriptor.has_value()) {
      ++result.contract_violations;
      continue;
    }
    if (item.status == ObservationStatus::kValid &&
        !valueMatches(*descriptor, *item.value)) {
      ++result.contract_violations;
      continue;
    }

    Observation observation;
    observation.entity = requested_item.entity;
    observation.metric = MetricId{requested_item.data_id};
    observation.value = std::move(item.value);
    observation.status = item.status;
    observation.scheduled_monotonic_time_ns =
        request.scheduled_time.time_since_epoch().count();
    observation.source_sample_time_ns = item.source_sample_time_ns;
    observation.observed_monotonic_time_ns = observed_time;
    observation.metric_semantic_version = descriptor->semantic_version;
    observation.observed_wall_time_ns = wall_time;
    observation.catalog_generation = request.catalog_generation;
    observation.source.provider = job.provider_id;
    observation.source.native_source = std::move(item.native_source);
    observation.error.status = std::move(error);
    observation.error.native_code = item.native_code;
    observation.error.retryable = item.retryable;
    observations.push_back(std::move(observation));
  }

  result.normalized_items = observations.size();
  if (observations.empty()) {
    result.status =
        Status(PDCM_STATUS_INTERNAL, "provider returned no committable items");
    return result;
  }

  result.commit = data_.commit(observations);
  if (!result.commit.status.ok() &&
      result.commit.status.code() != PDCM_STATUS_PARTIAL_RESULT) {
    result.status = result.commit.status;
    return result;
  }
  if (!usableCallStatus(provider_result.call_status.code())) {
    result.status = provider_result.call_status;
  } else if (result.contract_violations != 0) {
    result.status = Status(PDCM_STATUS_PARTIAL_RESULT,
                           "provider result violated the read contract");
  } else {
    result.status = result.commit.status;
  }
  return result;
}

} // namespace pdcm
