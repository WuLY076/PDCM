#include "collection/collection_coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

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

Status CollectionLimits::validate() const {
  if (max_plan_jobs == 0 || max_batch_items == 0 || max_result_items == 0 ||
      max_result_bytes == 0 || max_result_items < max_batch_items) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "collection limits must be positive and consistent");
  }
  return Status::success();
}

CollectionCoordinator::CollectionCoordinator(ProviderManager &provider,
                                             DataManager &data,
                                             std::shared_ptr<const Clock> clock,
                                             CollectionLimits limits)
    : provider_(provider), data_(data), clock_(std::move(clock)),
      limits_(limits) {
  if (clock_ == nullptr) {
    throw std::invalid_argument("CollectionCoordinator requires a clock");
  }
  const Status status = limits_.validate();
  if (!status.ok()) {
    throw std::invalid_argument(status.message());
  }
  plan_ = std::make_shared<CollectionPlan>();
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
    groups[group].push_back(CollectionPlanItem{
        watch.key, watch.period, watch.freshness, watch.priority});
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
  std::atomic_store_explicit(
      &plan_, std::static_pointer_cast<const CollectionPlan>(next),
      std::memory_order_release);
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

  std::uint64_t request_id = next_request_id_.load(std::memory_order_relaxed);
  while (request_id != 0 &&
         request_id != std::numeric_limits<std::uint64_t>::max() &&
         !next_request_id_.compare_exchange_weak(request_id, request_id + 1,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
  }
  if (request_id == 0 ||
      request_id == std::numeric_limits<std::uint64_t>::max()) {
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
  return normalizeAndCommit(*current, *found, request, provider_result);
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
