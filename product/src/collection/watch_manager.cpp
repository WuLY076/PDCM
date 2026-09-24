#include "collection/watch_manager.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace pdcm {

bool operator<(const WatchOwner &lhs, const WatchOwner &rhs) noexcept {
  return std::tie(lhs.kind, lhs.id) < std::tie(rhs.kind, rhs.id);
}

bool operator==(const EffectiveWatchKey &lhs,
                const EffectiveWatchKey &rhs) noexcept {
  return lhs.provider_id == rhs.provider_id && lhs.entity == rhs.entity &&
         lhs.kind == rhs.kind && lhs.data_id == rhs.data_id &&
         lhs.isolation_class == rhs.isolation_class;
}

bool operator<(const EffectiveWatchKey &lhs,
               const EffectiveWatchKey &rhs) noexcept {
  return std::tie(lhs.provider_id, lhs.entity.kind, lhs.entity.id.value,
                  lhs.entity.generation, lhs.kind, lhs.data_id,
                  lhs.isolation_class) <
         std::tie(rhs.provider_id, rhs.entity.kind, rhs.entity.id.value,
                  rhs.entity.generation, rhs.kind, rhs.data_id,
                  rhs.isolation_class);
}

Status WatchLimits::validate() const {
  if (max_logical_watches == 0 || max_effective_watches == 0 ||
      max_metrics_per_watch == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "watch limits must be non-zero");
  }
  return Status::success();
}

WatchManager::WatchManager(WatchLimits limits) : limits_(limits) {
  const Status status = limits_.validate();
  if (!status.ok()) {
    throw std::invalid_argument(status.message());
  }
  snapshot_ = std::make_shared<WatchSnapshot>();
}

Status WatchManager::activateCatalog(std::shared_ptr<const CatalogView> catalog,
                                     const TargetCatalog &target_catalog) {
  if (catalog == nullptr || catalog->generation() == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "watch catalog snapshot is invalid");
  }
  const Status target_status = target_catalog.validate();
  if (!target_status.ok()) {
    return target_status;
  }

  CatalogState next_catalog;
  next_catalog.view = std::move(catalog);
  next_catalog.topology_unsupported = next_catalog.view->topologyUnsupported();
  for (const MetricDescriptor &descriptor : target_catalog.metrics) {
    next_catalog.descriptors.emplace(descriptor.id.value, descriptor);
  }
  if (!next_catalog.topology_unsupported &&
      next_catalog.view->entities().size() == 1) {
    const EntityRecord &entity = next_catalog.view->entities().front();
    next_catalog.entity = entity.ref;
    next_catalog.provider_id = entity.provider_version;
    const CapabilityQueryResult capabilities =
        next_catalog.view->capabilities(entity.ref);
    if (capabilities.status.ok() && capabilities.capabilities.has_value()) {
      for (const CapabilityItem &item : capabilities.capabilities->items) {
        if (item.kind == CapabilityKind::kMetric) {
          next_catalog.supported[item.id] = item.supported;
        }
      }
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (catalog_.view != nullptr &&
      next_catalog.view->generation() < catalog_.view->generation()) {
    return Status(PDCM_STATUS_STALE_GENERATION,
                  "watch catalog generation moved backward");
  }
  if (catalog_.view != nullptr &&
      next_catalog.view->generation() == catalog_.view->generation()) {
    return Status::success();
  }

  catalog_ = std::move(next_catalog);
  std::map<WatchId, LogicalWatch> retained;
  for (const auto &entry : logical_) {
    if (catalog_.entity.has_value() &&
        entry.second.requirement.catalog_generation ==
            catalog_.view->generation() &&
        entry.second.requirement.entity == *catalog_.entity) {
      retained.emplace(entry);
    }
  }
  return publish(std::move(retained));
}

Status WatchManager::validateRequirement(
    const WatchRequirement &requirement,
    std::vector<MetricId> &supported_metrics,
    std::vector<MetricId> &unsupported_metrics) const {
  if (catalog_.view == nullptr) {
    return Status(PDCM_STATUS_NOT_INITIALIZED, "watch catalog is not active");
  }
  if (catalog_.topology_unsupported || !catalog_.entity.has_value()) {
    return Status(PDCM_STATUS_UNSUPPORTED,
                  "watch requires exactly one active device");
  }
  if (requirement.catalog_generation != catalog_.view->generation() ||
      requirement.entity != *catalog_.entity) {
    return Status(PDCM_STATUS_STALE_GENERATION,
                  "watch catalog or entity generation is stale");
  }
  if (requirement.metrics.empty() ||
      requirement.metrics.size() > limits_.max_metrics_per_watch ||
      requirement.period.count() <= 0 ||
      requirement.freshness < requirement.period ||
      requirement.retention < requirement.freshness) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "watch timing or selection is invalid");
  }

  std::set<std::uint32_t> unique;
  for (const MetricId metric : requirement.metrics) {
    if (metric.value == 0 || !unique.insert(metric.value).second) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "watch metrics contain invalid or duplicate IDs");
    }
    const auto descriptor = catalog_.descriptors.find(metric.value);
    const auto capability = catalog_.supported.find(metric.value);
    const bool supported =
        descriptor != catalog_.descriptors.end() &&
        descriptor->second.collection_mode == MetricCollectionMode::kPoll &&
        capability != catalog_.supported.end() && capability->second;
    if (!supported) {
      unsupported_metrics.push_back(metric);
      continue;
    }
    if (requirement.period.count() <
        static_cast<std::int64_t>(descriptor->second.min_period_ns)) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "watch period is below metric minimum");
    }
    supported_metrics.push_back(metric);
  }
  return Status::success();
}

std::shared_ptr<const WatchSnapshot>
WatchManager::buildSnapshot(const std::map<WatchId, LogicalWatch> &logical,
                            const std::uint64_t version) const {
  auto snapshot = std::make_shared<WatchSnapshot>();
  snapshot->version = version;
  snapshot->catalog_generation =
      catalog_.view == nullptr ? 0 : catalog_.view->generation();
  std::map<EffectiveWatchKey, EffectiveWatch> effective;

  for (const auto &entry : logical) {
    snapshot->logical_watches.push_back(entry.second);
    for (const MetricId metric : entry.second.requirement.metrics) {
      EffectiveWatchKey key;
      key.provider_id = catalog_.provider_id;
      key.entity = entry.second.requirement.entity;
      key.kind = ProviderDataKind::kMetric;
      key.data_id = metric.value;
      EffectiveWatch &merged = effective[key];
      if (merged.logical_watches.empty()) {
        merged.key = key;
        merged.period = entry.second.requirement.period;
        merged.freshness = entry.second.requirement.freshness;
        merged.retention = entry.second.requirement.retention;
        merged.priority = entry.second.requirement.priority;
      } else {
        merged.period =
            std::min(merged.period, entry.second.requirement.period);
        merged.freshness =
            std::min(merged.freshness, entry.second.requirement.freshness);
        merged.retention =
            std::max(merged.retention, entry.second.requirement.retention);
        merged.priority =
            std::max(merged.priority, entry.second.requirement.priority);
      }
      merged.logical_watches.push_back(entry.first);
    }
  }
  for (auto &entry : effective) {
    snapshot->effective_watches.push_back(std::move(entry.second));
  }
  return snapshot;
}

Status WatchManager::publish(std::map<WatchId, LogicalWatch> next) {
  if (next.size() > limits_.max_logical_watches) {
    return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                  "logical watch limit exceeded");
  }
  const std::uint64_t current_version =
      snapshot_ == nullptr ? 0 : snapshot_->version;
  if (current_version == std::numeric_limits<std::uint64_t>::max()) {
    return Status(PDCM_STATUS_INTERNAL, "watch snapshot version overflow");
  }
  std::shared_ptr<const WatchSnapshot> next_snapshot =
      buildSnapshot(next, current_version + 1);
  if (next_snapshot->effective_watches.size() > limits_.max_effective_watches) {
    return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                  "effective watch limit exceeded");
  }
  logical_ = std::move(next);
  for (auto iterator = sample_counts_.begin();
       iterator != sample_counts_.end();) {
    if (logical_.find(iterator->first) == logical_.end()) {
      last_sample_times_.erase(iterator->first);
      iterator = sample_counts_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  for (auto iterator = last_sample_times_.begin();
       iterator != last_sample_times_.end();) {
    if (logical_.find(iterator->first) == logical_.end()) {
      iterator = last_sample_times_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  std::atomic_store_explicit(&snapshot_, std::move(next_snapshot),
                             std::memory_order_release);
  return Status::success();
}

WatchCreateResult WatchManager::create(const WatchOwner &owner,
                                       WatchRequirement requirement) {
  WatchCreateResult result;
  if (owner.kind == WatchOwnerKind::kSession && owner.id == 0) {
    result.status =
        Status(PDCM_STATUS_INVALID_ARGUMENT, "watch owner or ID is invalid");
    return result;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (next_watch_id_ == std::numeric_limits<WatchId>::max()) {
    result.status =
        Status(PDCM_STATUS_RESOURCE_EXHAUSTED, "watch ID space is exhausted");
    return result;
  }
  std::vector<MetricId> supported;
  result.status =
      validateRequirement(requirement, supported, result.unsupported_metrics);
  if (!result.status.ok()) {
    return result;
  }
  if (!result.unsupported_metrics.empty() && !requirement.allow_partial) {
    result.status =
        Status(PDCM_STATUS_UNSUPPORTED, "watch contains unsupported metrics");
    return result;
  }
  if (supported.empty()) {
    result.status =
        Status(PDCM_STATUS_UNSUPPORTED, "watch has no supported metrics");
    return result;
  }

  std::sort(supported.begin(), supported.end(),
            [](const MetricId lhs, const MetricId rhs) {
              return lhs.value < rhs.value;
            });
  requirement.metrics = std::move(supported);
  const WatchId id = next_watch_id_;
  std::map<WatchId, LogicalWatch> next = logical_;
  next.emplace(id, LogicalWatch{id, owner, std::move(requirement)});
  const Status published = publish(std::move(next));
  if (!published.ok()) {
    result.status = published;
    return result;
  }
  ++next_watch_id_;
  result.watch_id = id;
  result.status = result.unsupported_metrics.empty()
                      ? Status::success()
                      : Status(PDCM_STATUS_PARTIAL_RESULT,
                               "watch omitted unsupported metrics");
  return result;
}

Status WatchManager::destroy(const WatchOwner &owner, const WatchId watch_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = logical_.find(watch_id);
  if (found == logical_.end()) {
    return Status(PDCM_STATUS_NOT_FOUND, "watch does not exist");
  }
  if (!(found->second.owner == owner)) {
    return Status(PDCM_STATUS_PERMISSION_DENIED,
                  "watch belongs to another owner");
  }
  std::map<WatchId, LogicalWatch> next = logical_;
  next.erase(watch_id);
  return publish(std::move(next));
}

Status WatchManager::removeOwner(const WatchOwner &owner) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::map<WatchId, LogicalWatch> next = logical_;
  for (auto iterator = next.begin(); iterator != next.end();) {
    if (iterator->second.owner == owner) {
      iterator = next.erase(iterator);
    } else {
      ++iterator;
    }
  }
  if (next.size() == logical_.size()) {
    return Status(PDCM_STATUS_NOT_FOUND, "watch owner has no requirements");
  }
  return publish(std::move(next));
}

Status WatchManager::recordSamples(std::vector<WatchId> watch_ids,
                                   const MonotonicTime first_scheduled_time,
                                   const Nanoseconds period,
                                   const std::uint64_t sample_count) {
  if (watch_ids.empty() ||
      first_scheduled_time.time_since_epoch().count() < 0 ||
      period.count() <= 0 || sample_count == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "logical sample token arguments are invalid");
  }
  std::sort(watch_ids.begin(), watch_ids.end());
  if (std::adjacent_find(watch_ids.begin(), watch_ids.end()) !=
      watch_ids.end()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "logical sample token contains duplicate watch IDs");
  }
  const std::uint64_t intervals = sample_count - 1;
  const auto max_time = std::numeric_limits<std::int64_t>::max();
  if (intervals > static_cast<std::uint64_t>(max_time / period.count())) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "logical sample token time range overflows");
  }
  const std::int64_t delta =
      period.count() * static_cast<std::int64_t>(intervals);
  if (first_scheduled_time.time_since_epoch().count() > max_time - delta) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "logical sample token time range overflows");
  }
  const MonotonicTime last_scheduled_time =
      first_scheduled_time + Nanoseconds{delta};

  std::lock_guard<std::mutex> lock(mutex_);
  std::map<WatchId, LogicalWatch> next = logical_;
  std::map<WatchId, std::uint64_t> next_counts = sample_counts_;
  std::map<WatchId, MonotonicTime> next_times = last_sample_times_;
  bool found_any = false;
  bool changed = false;
  bool removed = false;

  for (const WatchId watch_id : watch_ids) {
    const auto watch = next.find(watch_id);
    if (watch == next.end()) {
      continue;
    }
    found_any = true;

    std::uint64_t skipped = 0;
    const auto previous = next_times.find(watch_id);
    if (previous != next_times.end() &&
        previous->second >= first_scheduled_time) {
      const std::int64_t elapsed =
          (previous->second - first_scheduled_time).count();
      skipped = static_cast<std::uint64_t>(elapsed / period.count()) + 1;
      if (skipped >= sample_count) {
        continue;
      }
    }

    const std::uint64_t added = sample_count - skipped;
    const std::uint64_t current = next_counts[watch_id];
    const std::uint64_t updated =
        added > std::numeric_limits<std::uint64_t>::max() - current
            ? std::numeric_limits<std::uint64_t>::max()
            : current + added;
    changed = true;

    const std::uint64_t limit = watch->second.requirement.sample_limit;
    if (limit != 0 && updated >= limit) {
      next.erase(watch);
      next_counts.erase(watch_id);
      next_times.erase(watch_id);
      removed = true;
    } else {
      next_counts[watch_id] = updated;
      next_times[watch_id] = last_scheduled_time;
    }
  }

  if (!found_any) {
    return Status(PDCM_STATUS_NOT_FOUND,
                  "logical sample token has no active watches");
  }
  if (!changed) {
    return Status::success();
  }
  if (removed) {
    const Status status = publish(std::move(next));
    if (!status.ok()) {
      return status;
    }
  }
  sample_counts_ = std::move(next_counts);
  last_sample_times_ = std::move(next_times);
  return Status::success();
}
std::shared_ptr<const WatchSnapshot> WatchManager::snapshot() const noexcept {
  return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
}

} // namespace pdcm
