#include "data/data_manager.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>

namespace pdcm {
namespace {

bool sameEntity(const EntityRef &lhs, const EntityRef &rhs) {
  return lhs == rhs;
}

bool sameLogicalEntity(const EntityRef &lhs, const EntityRef &rhs) {
  return lhs.kind == rhs.kind && lhs.id == rhs.id;
}

std::size_t valueBytes(const MetricValue &value) {
  return std::visit(
      [](const auto &typed_value) -> std::size_t {
        using ValueType = std::decay_t<decltype(typed_value)>;
        if constexpr (std::is_same_v<ValueType, std::string>) {
          return typed_value.size();
        } else {
          return sizeof(ValueType);
        }
      },
      value);
}

} // namespace

bool operator<(const DataKey &lhs, const DataKey &rhs) noexcept {
  return std::tie(lhs.entity.kind, lhs.entity.id.value, lhs.entity.generation,
                  lhs.metric.value) <
         std::tie(rhs.entity.kind, rhs.entity.id.value, rhs.entity.generation,
                  rhs.metric.value);
}

Status DataStoreLimits::validate() const {
  if (shard_count == 0 || shard_count > 256 || max_keys == 0 ||
      max_history_samples_per_key == 0 || max_history_duration_ns <= 0 ||
      max_total_bytes == 0 || max_value_bytes == 0 || max_query_items == 0 ||
      max_tombstones == 0 || max_value_bytes > max_total_bytes) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "data store limits must be positive and bounded");
  }
  return Status::success();
}

DataManager::DataManager(DataStoreLimits limits) : limits_(limits) {
  const Status validation = limits_.validate();
  if (!validation.ok()) {
    throw std::invalid_argument(validation.message());
  }
  shards_.reserve(limits_.shard_count);
  for (std::size_t index = 0; index < limits_.shard_count; ++index) {
    shards_.push_back(std::make_unique<Shard>());
  }
}

Status DataManager::activateCatalog(std::shared_ptr<const CatalogView> catalog,
                                    const TargetCatalog &target_catalog) {
  if (!catalog || catalog->generation() == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "data catalog snapshot is invalid");
  }
  const Status target_status = target_catalog.validate();
  if (!target_status.ok()) {
    return target_status;
  }
  for (const EntityRecord &entity : catalog->entities()) {
    if (entity.target != target_catalog.target ||
        entity.ref.kind != EntityKind::kDevice || entity.ref.generation == 0) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "data catalog entity is invalid");
    }
  }

  std::unique_lock<std::mutex> state_lock(state_mutex_);
  std::vector<std::unique_lock<std::mutex>> shard_locks;
  shard_locks.reserve(shards_.size());
  for (const std::unique_ptr<Shard> &shard : shards_) {
    shard_locks.emplace_back(shard->mutex);
  }

  if (catalog_.generation != 0 && catalog->generation() < catalog_.generation) {
    return Status(PDCM_STATUS_STALE_GENERATION,
                  "data catalog generation moved backward");
  }
  if (catalog->generation() == catalog_.generation) {
    return Status::success();
  }

  for (const EntityRef &old_entity : catalog_.entities) {
    const bool still_present =
        std::any_of(catalog->entities().begin(), catalog->entities().end(),
                    [&old_entity](const EntityRecord &candidate) {
                      return sameEntity(old_entity, candidate.ref);
                    });
    if (!still_present) {
      tombstones_.push_back(EntityTombstone{old_entity, catalog->generation()});
      while (tombstones_.size() > limits_.max_tombstones) {
        tombstones_.pop_front();
      }
    }
  }

  for (const std::unique_ptr<Shard> &shard : shards_) {
    shard->entries.clear();
  }

  CatalogState next;
  next.generation = catalog->generation();
  next.topology_unsupported = catalog->topologyUnsupported();
  next.entities.reserve(catalog->entities().size());
  for (const EntityRecord &entity : catalog->entities()) {
    next.entities.push_back(entity.ref);
  }
  for (const MetricDescriptor &descriptor : target_catalog.metrics) {
    next.metrics.emplace(descriptor.id.value, descriptor);
  }
  catalog_ = std::move(next);

  shard_locks.clear();
  state_lock.unlock();
  state_changed_.notify_all();
  return Status::success();
}

DataCommitResult
DataManager::commit(const std::vector<Observation> &observations) {
  DataCommitResult result;
  if (observations.empty()) {
    result.status =
        Status(PDCM_STATUS_INVALID_ARGUMENT, "observation batch is empty");
    return result;
  }

  std::unique_lock<std::mutex> state_lock(state_mutex_);
  std::vector<std::unique_lock<std::mutex>> shard_locks;
  shard_locks.reserve(shards_.size());
  for (const std::unique_ptr<Shard> &shard : shards_) {
    shard_locks.emplace_back(shard->mutex);
  }

  if (catalog_.generation == 0) {
    result.status =
        Status(PDCM_STATUS_NOT_INITIALIZED, "data catalog is not active");
    return result;
  }
  if (catalog_.topology_unsupported) {
    result.status =
        Status(PDCM_STATUS_UNSUPPORTED, "data commit rejects multi-device P0");
    return result;
  }
  if (commit_epoch_ == std::numeric_limits<std::uint64_t>::max()) {
    result.status = Status(PDCM_STATUS_INTERNAL, "data commit epoch overflow");
    return result;
  }

  std::set<DataKey> unique_keys;
  for (const Observation &observation : observations) {
    const DataKey key{observation.entity, observation.metric};
    if (!unique_keys.insert(key).second) {
      result.status =
          Status(PDCM_STATUS_INVALID_ARGUMENT, "duplicate observation key");
      return result;
    }
    if (observation.commit_epoch != 0 ||
        observation.catalog_generation != catalog_.generation ||
        observation.status == ObservationStatus::kStale) {
      result.status = Status(PDCM_STATUS_STALE_GENERATION,
                             "observation generation or epoch is invalid");
      return result;
    }
    const bool entity_present =
        std::any_of(catalog_.entities.begin(), catalog_.entities.end(),
                    [&observation](const EntityRef &candidate) {
                      return sameEntity(candidate, observation.entity);
                    });
    if (!entity_present) {
      result.status =
          Status(PDCM_STATUS_STALE_GENERATION, "observation entity is stale");
      return result;
    }
    const auto descriptor = catalog_.metrics.find(observation.metric.value);
    if (descriptor == catalog_.metrics.end()) {
      result.status =
          Status(PDCM_STATUS_UNSUPPORTED, "observation metric is unknown");
      return result;
    }
    const Status validation = validateObservation(observation);
    if (!validation.ok() ||
        observation.metric_semantic_version !=
            descriptor->second.semantic_version ||
        (observation.value.has_value() &&
         !valueMatches(descriptor->second, *observation.value)) ||
        (observation.value.has_value() &&
         valueBytes(*observation.value) > limits_.max_value_bytes)) {
      result.status = Status(PDCM_STATUS_INVALID_ARGUMENT,
                             "observation does not match metric contract");
      return result;
    }
  }

  std::vector<std::map<DataKey, Entry>> staged;
  staged.reserve(shards_.size());
  for (const std::unique_ptr<Shard> &shard : shards_) {
    staged.push_back(shard->entries);
  }

  const std::uint64_t next_epoch = commit_epoch_ + 1;
  bool partial = false;
  for (Observation observation : observations) {
    observation.commit_epoch = next_epoch;
    const DataKey key{observation.entity, observation.metric};
    Entry &entry = staged[shardIndex(key)][key];
    if (entry.latest.has_value() &&
        observation.observed_monotonic_time_ns <
            entry.latest->observed_monotonic_time_ns) {
      result.status = Status(PDCM_STATUS_INVALID_ARGUMENT,
                             "observation time moved backward");
      return result;
    }

    entry.latest = observation;
    if (observation.status == ObservationStatus::kValid) {
      entry.last_valid = observation;
      entry.history.push_back(observation);
      while (entry.history.size() > limits_.max_history_samples_per_key) {
        entry.history.pop_front();
      }
      while (!entry.history.empty() &&
             observation.observed_monotonic_time_ns -
                     entry.history.front().observed_monotonic_time_ns >
                 limits_.max_history_duration_ns) {
        entry.history.pop_front();
      }
    } else {
      partial = true;
    }
  }

  std::size_t key_count = 0;
  std::size_t total_bytes = 0;
  for (const auto &shard : staged) {
    key_count += shard.size();
    for (const auto &pair : shard) {
      total_bytes += entryBytes(pair.second);
    }
  }
  if (key_count > limits_.max_keys) {
    result.status =
        Status(PDCM_STATUS_RESOURCE_EXHAUSTED, "data key limit exceeded");
    return result;
  }

  while (total_bytes > limits_.max_total_bytes) {
    Entry *oldest_entry = nullptr;
    std::int64_t oldest_time = std::numeric_limits<std::int64_t>::max();
    for (auto &shard : staged) {
      for (auto &pair : shard) {
        if (!pair.second.history.empty() &&
            pair.second.history.front().observed_monotonic_time_ns <
                oldest_time) {
          oldest_time = pair.second.history.front().observed_monotonic_time_ns;
          oldest_entry = &pair.second;
        }
      }
    }
    if (oldest_entry == nullptr) {
      break;
    }
    total_bytes -= observationBytes(oldest_entry->history.front());
    oldest_entry->history.pop_front();
  }
  if (total_bytes > limits_.max_total_bytes) {
    result.status =
        Status(PDCM_STATUS_RESOURCE_EXHAUSTED, "data byte limit exceeded");
    return result;
  }

  for (std::size_t index = 0; index < shards_.size(); ++index) {
    shards_[index]->entries.swap(staged[index]);
  }
  commit_epoch_ = next_epoch;
  result.status = partial ? Status(PDCM_STATUS_PARTIAL_RESULT,
                                   "observation batch contains failed items")
                          : Status::success();
  result.commit_epoch = next_epoch;
  result.committed_items = observations.size();

  shard_locks.clear();
  state_lock.unlock();
  state_changed_.notify_all();
  return result;
}

DataReadResult
DataManager::readLatest(std::vector<DataKey> keys, const bool allow_stale,
                        const std::int64_t now_monotonic_ns,
                        const std::uint64_t required_catalog_generation) const {
  DataReadResult result;
  if (keys.empty() || keys.size() > limits_.max_query_items ||
      now_monotonic_ns < 0) {
    result.status = Status(PDCM_STATUS_INVALID_ARGUMENT,
                           "data query arguments are invalid");
    return result;
  }
  std::sort(keys.begin(), keys.end());
  if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
    result.status =
        Status(PDCM_STATUS_INVALID_ARGUMENT, "data query contains duplicates");
    return result;
  }

  std::unique_lock<std::mutex> state_lock(state_mutex_);
  std::vector<std::unique_lock<std::mutex>> shard_locks;
  shard_locks.reserve(shards_.size());
  for (const std::unique_ptr<Shard> &shard : shards_) {
    shard_locks.emplace_back(shard->mutex);
  }

  result.catalog_generation = catalog_.generation;
  result.observed_commit_epoch = commit_epoch_;
  if (catalog_.generation == 0) {
    result.status =
        Status(PDCM_STATUS_NOT_INITIALIZED, "data catalog is not active");
    return result;
  }
  if (required_catalog_generation != 0 &&
      required_catalog_generation != catalog_.generation) {
    result.status = Status(PDCM_STATUS_STALE_GENERATION,
                           "query catalog generation is stale");
    return result;
  }
  if (catalog_.topology_unsupported) {
    result.status =
        Status(PDCM_STATUS_UNSUPPORTED, "data query rejects multi-device P0");
    return result;
  }

  result.items.reserve(keys.size());
  bool partial = false;
  for (const DataKey &key : keys) {
    const auto current_entity =
        std::find_if(catalog_.entities.begin(), catalog_.entities.end(),
                     [&key](const EntityRef &candidate) {
                       return sameLogicalEntity(candidate, key.entity);
                     });
    if (current_entity == catalog_.entities.end()) {
      result.status =
          Status(PDCM_STATUS_NOT_FOUND, "query entity is not present");
      result.items.clear();
      return result;
    }
    if (*current_entity != key.entity) {
      result.status = Status(PDCM_STATUS_STALE_GENERATION,
                             "query entity generation is stale");
      result.items.clear();
      return result;
    }

    const auto descriptor = catalog_.metrics.find(key.metric.value);
    if (descriptor == catalog_.metrics.end()) {
      result.items.push_back(unavailable(
          key, MetricDescriptor{}, catalog_.generation, commit_epoch_,
          now_monotonic_ns, PDCM_STATUS_UNSUPPORTED,
          ObservationStatus::kUnsupported, "metric is not in active catalog"));
      partial = true;
      continue;
    }

    const auto stored = shards_[shardIndex(key)]->entries.find(key);
    if (stored == shards_[shardIndex(key)]->entries.end() ||
        !stored->second.latest.has_value()) {
      result.items.push_back(unavailable(
          key, descriptor->second, catalog_.generation, commit_epoch_,
          now_monotonic_ns, PDCM_STATUS_UNAVAILABLE,
          ObservationStatus::kNotAvailable, "metric has not been sampled"));
      partial = true;
      continue;
    }

    Observation item = *stored->second.latest;
    if (item.status != ObservationStatus::kValid && allow_stale &&
        stored->second.last_valid.has_value()) {
      if (now_monotonic_ns <
          stored->second.last_valid->observed_monotonic_time_ns) {
        result.status =
            Status(PDCM_STATUS_INVALID_ARGUMENT, "query time moved backward");
        result.items.clear();
        return result;
      }
      const ErrorMetadata latest_failure = item.error;
      item = *stored->second.last_valid;
      item.status = ObservationStatus::kStale;
      item.commit_epoch = stored->second.latest->commit_epoch;
      item.stale_age_ns = now_monotonic_ns - item.observed_monotonic_time_ns;
      item.latest_failure = latest_failure;
      item.error = latest_failure;
    }
    if (item.status != ObservationStatus::kValid) {
      partial = true;
    }
    result.items.push_back(std::move(item));
  }

  result.status = partial ? Status(PDCM_STATUS_PARTIAL_RESULT,
                                   "data query contains non-valid items")
                          : Status::success();
  return result;
}

DataHistoryResult DataManager::history(const DataKey &key) const {
  DataHistoryResult result;
  std::unique_lock<std::mutex> state_lock(state_mutex_);
  std::vector<std::unique_lock<std::mutex>> shard_locks;
  shard_locks.reserve(shards_.size());
  for (const std::unique_ptr<Shard> &shard : shards_) {
    shard_locks.emplace_back(shard->mutex);
  }

  if (catalog_.generation == 0) {
    result.status =
        Status(PDCM_STATUS_NOT_INITIALIZED, "data catalog is not active");
    return result;
  }
  const auto stored = shards_[shardIndex(key)]->entries.find(key);
  if (stored == shards_[shardIndex(key)]->entries.end()) {
    result.status =
        Status(PDCM_STATUS_NOT_FOUND, "data history is not present");
    return result;
  }
  result.samples.assign(stored->second.history.begin(),
                        stored->second.history.end());
  result.status = Status::success();
  return result;
}

Status DataManager::waitForEpoch(const std::uint64_t epoch,
                                 const MonotonicTime deadline) const {
  if (epoch == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT, "wait epoch must be non-zero");
  }
  std::unique_lock<std::mutex> lock(state_mutex_);
  if (!state_changed_.wait_until(
          lock, deadline, [this, epoch] { return commit_epoch_ >= epoch; })) {
    return Status(PDCM_STATUS_TIMEOUT, "data epoch wait expired");
  }
  return Status::success();
}

std::uint64_t DataManager::catalogGeneration() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return catalog_.generation;
}

std::uint64_t DataManager::commitEpoch() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return commit_epoch_;
}

std::size_t DataManager::keyCount() const {
  std::unique_lock<std::mutex> state_lock(state_mutex_);
  std::vector<std::unique_lock<std::mutex>> shard_locks;
  shard_locks.reserve(shards_.size());
  for (const std::unique_ptr<Shard> &shard : shards_) {
    shard_locks.emplace_back(shard->mutex);
  }
  std::size_t count = 0;
  for (const std::unique_ptr<Shard> &shard : shards_) {
    count += shard->entries.size();
  }
  return count;
}

std::size_t DataManager::storedBytes() const {
  std::unique_lock<std::mutex> state_lock(state_mutex_);
  std::vector<std::unique_lock<std::mutex>> shard_locks;
  shard_locks.reserve(shards_.size());
  for (const std::unique_ptr<Shard> &shard : shards_) {
    shard_locks.emplace_back(shard->mutex);
  }
  std::size_t bytes = 0;
  for (const std::unique_ptr<Shard> &shard : shards_) {
    for (const auto &pair : shard->entries) {
      bytes += entryBytes(pair.second);
    }
  }
  return bytes;
}

std::vector<EntityTombstone> DataManager::tombstones() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return {tombstones_.begin(), tombstones_.end()};
}

std::size_t DataManager::shardIndex(const DataKey &key) const noexcept {
  const std::uint64_t mixed =
      key.entity.id.value ^
      (key.entity.generation + UINT64_C(0x9E3779B97F4A7C15)) ^
      static_cast<std::uint64_t>(key.metric.value);
  return static_cast<std::size_t>(mixed % shards_.size());
}

bool DataManager::valueMatches(const MetricDescriptor &descriptor,
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

std::size_t DataManager::observationBytes(const Observation &observation) {
  std::size_t bytes = sizeof(Observation);
  if (observation.value.has_value()) {
    bytes += valueBytes(*observation.value);
  }
  bytes += observation.source.provider.size();
  bytes += observation.source.native_source.size();
  bytes += observation.error.status.message().size();
  bytes += observation.derivation.processor_id.size();
  bytes +=
      observation.derivation.input_sequences.size() * sizeof(std::uint64_t);
  if (observation.latest_failure.has_value()) {
    bytes += observation.latest_failure->status.message().size();
  }
  return bytes;
}

std::size_t DataManager::entryBytes(const Entry &entry) {
  std::size_t bytes = sizeof(Entry);
  if (entry.latest.has_value()) {
    bytes += observationBytes(*entry.latest);
  }
  if (entry.last_valid.has_value()) {
    bytes += observationBytes(*entry.last_valid);
  }
  for (const Observation &sample : entry.history) {
    bytes += observationBytes(sample);
  }
  return bytes;
}

Observation DataManager::unavailable(
    const DataKey &key, const MetricDescriptor &descriptor,
    const std::uint64_t catalog_generation, const std::uint64_t commit_epoch,
    const std::int64_t now_monotonic_ns, const pdcm_status_t status,
    const ObservationStatus observation_status, const char *const message) {
  Observation observation;
  observation.entity = key.entity;
  observation.metric = key.metric;
  observation.status = observation_status;
  observation.metric_semantic_version =
      descriptor.semantic_version == 0 ? 1 : descriptor.semantic_version;
  observation.observed_monotonic_time_ns = now_monotonic_ns;
  observation.catalog_generation = catalog_generation;
  observation.commit_epoch = commit_epoch;
  observation.source.provider = "pdcm-data";
  observation.error.status = Status(status, message);
  return observation;
}

} // namespace pdcm
#include <type_traits>
