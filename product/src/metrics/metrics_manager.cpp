#include "metrics/metrics_manager.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace pdcm {
namespace {

std::int64_t monotonicNanoseconds(const MonotonicTime time) {
  return time.time_since_epoch().count();
}

EvidenceRef evidenceReference(const FirmwareHeartbeatEvidence &evidence) {
  return EvidenceRef{evidence.evidence_id, evidence.sequence_or_token,
                     evidence.source_sample_time_ns,
                     evidence.observed_monotonic_time_ns, evidence.source};
}

std::uint64_t effectiveFreshness(const std::uint64_t catalog_freshness,
                                 const std::uint64_t requested_max_age) {
  if (requested_max_age == 0) {
    return catalog_freshness;
  }
  return std::min(catalog_freshness, requested_max_age);
}

} // namespace

MetricsManager::MetricsManager(
    DataManager &data_manager, const Clock &clock,
    std::vector<std::shared_ptr<const MetricProcessor>> processors)
    : data_manager_(data_manager), clock_(clock),
      processors_(std::move(processors)) {}

Status
MetricsManager::activateCatalog(std::shared_ptr<const CatalogView> catalog,
                                const TargetCatalog &target_catalog) {
  if (!catalog || catalog->generation() == 0 ||
      data_manager_.catalogGeneration() != catalog->generation()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "metrics catalog is not active in DataManager");
  }
  const Status target_status = target_catalog.validate();
  if (!target_status.ok()) {
    return target_status;
  }
  const Status graph_status = processor_graph_.rebuild(
      target_catalog, catalog->generation(), processors_);
  if (!graph_status.ok()) {
    return graph_status;
  }

  Runtime next;
  next.catalog_generation = catalog->generation();
  next.heartbeat = target_catalog.health.front();
  if (catalog->topologyUnsupported()) {
    std::lock_guard<std::mutex> lock(mutex_);
    runtime_ = std::move(next);
    return Status(PDCM_STATUS_UNSUPPORTED,
                  "metrics health rejects multi-device P0");
  }
  if (catalog->entities().size() > 1) {
    return Status(PDCM_STATUS_UNSUPPORTED,
                  "metrics health accepts at most one device");
  }
  if (!catalog->entities().empty()) {
    next.entity = catalog->entities().front().ref;
    const CapabilityQueryResult capabilities =
        catalog->capabilities(*next.entity);
    if (!capabilities.status.ok() || !capabilities.capabilities.has_value()) {
      return capabilities.status;
    }
    const auto heartbeat = std::find_if(
        capabilities.capabilities->items.begin(),
        capabilities.capabilities->items.end(), [](const CapabilityItem &item) {
          return item.kind == CapabilityKind::kHealth &&
                 item.id == kFirmwareHeartbeatHealthId;
        });
    next.heartbeat_supported =
        heartbeat != capabilities.capabilities->items.end() &&
        heartbeat->supported;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (runtime_.catalog_generation > next.catalog_generation) {
      return Status(PDCM_STATUS_STALE_GENERATION,
                    "metrics catalog generation moved backward");
    }
    runtime_ = next;
  }

  if (!next.entity.has_value()) {
    return Status::success();
  }
  const std::int64_t now = monotonicNanoseconds(clock_.monotonicNow());
  HealthResult initial =
      next.heartbeat_supported
          ? unknown(next, StableHealthCode::kHeartbeatMissing,
                    ObservationStatus::kNotAvailable, now,
                    HealthLimitation{HealthLimitationCode::kEvidenceMissing,
                                     "firmware heartbeat has not been sampled"})
          : unknown(
                next, StableHealthCode::kHeartbeatUnsupported,
                ObservationStatus::kUnsupported, now,
                HealthLimitation{HealthLimitationCode::kNativeMappingPending,
                                 next.heartbeat.limitation});
  return data_manager_.commitHealth(std::move(initial)).status;
}

Status
MetricsManager::onHeartbeatEvidenceCommitted(const std::uint64_t evidence_id) {
  const Runtime runtime = runtimeSnapshot();
  if (runtime.catalog_generation == 0 || !runtime.entity.has_value()) {
    return Status(PDCM_STATUS_NOT_INITIALIZED,
                  "metrics health catalog is not active");
  }
  const EvidenceReadResult read = data_manager_.heartbeatEvidence(evidence_id);
  if (!read.status.ok() || !read.evidence.has_value()) {
    return read.status;
  }
  if (read.evidence->catalog_generation != runtime.catalog_generation ||
      read.evidence->entity != *runtime.entity) {
    return Status(PDCM_STATUS_STALE_GENERATION,
                  "heartbeat evidence does not match metrics catalog");
  }
  const std::int64_t now = monotonicNanoseconds(clock_.monotonicNow());
  if (now < read.evidence->observed_monotonic_time_ns) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "heartbeat evaluation time moved backward");
  }
  return data_manager_
      .commitHealth(evaluate(runtime, *read.evidence, now,
                             runtime.heartbeat.freshness_ns))
      .status;
}

Status MetricsManager::onProviderStateChanged(const ProviderState state) {
  if (state == ProviderState::kReady) {
    return Status::success();
  }
  const Runtime runtime = runtimeSnapshot();
  if (runtime.catalog_generation == 0 || !runtime.entity.has_value()) {
    return Status(PDCM_STATUS_NOT_INITIALIZED,
                  "metrics health catalog is not active");
  }
  const std::int64_t now = monotonicNanoseconds(clock_.monotonicNow());
  HealthResult result =
      unknown(runtime, StableHealthCode::kProviderUnavailable,
              ObservationStatus::kError, now,
              HealthLimitation{HealthLimitationCode::kProviderUnavailable,
                               "provider is unavailable"});
  const EvidenceReadResult latest = data_manager_.latestHeartbeatEvidence(
      *runtime.entity, runtime.catalog_generation);
  if (latest.status.ok() && latest.evidence.has_value() &&
      now >= latest.evidence->observed_monotonic_time_ns) {
    result.evidence.push_back(evidenceReference(*latest.evidence));
    result.evidence_age_ns = now - latest.evidence->observed_monotonic_time_ns;
  }
  return data_manager_.commitHealth(std::move(result)).status;
}

Status MetricsManager::runFreshnessOnce(const MonotonicTime now) {
  const Runtime runtime = runtimeSnapshot();
  if (runtime.catalog_generation == 0 || !runtime.entity.has_value()) {
    return Status(PDCM_STATUS_NOT_INITIALIZED,
                  "metrics health catalog is not active");
  }
  const std::int64_t now_ns = monotonicNanoseconds(now);
  if (now_ns < 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "freshness evaluation time is invalid");
  }
  const HealthReadResult current =
      data_manager_.readHealth(*runtime.entity, runtime.catalog_generation);
  if (!current.status.ok() || !current.result.has_value()) {
    return current.status;
  }
  if (!isDeterminateHealth(current.result->state)) {
    return Status::success();
  }
  const EvidenceReadResult latest = data_manager_.latestHeartbeatEvidence(
      *runtime.entity, runtime.catalog_generation);
  if (!latest.status.ok() || !latest.evidence.has_value()) {
    return latest.status;
  }
  if (now_ns < latest.evidence->observed_monotonic_time_ns) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "freshness evaluation time moved backward");
  }
  const std::int64_t age = now_ns - latest.evidence->observed_monotonic_time_ns;
  if (static_cast<std::uint64_t>(age) <= runtime.heartbeat.freshness_ns) {
    return Status::success();
  }
  return data_manager_
      .commitHealth(evaluate(runtime, *latest.evidence, now_ns,
                             runtime.heartbeat.freshness_ns))
      .status;
}

HealthQueryResult
MetricsManager::queryHealth(const HealthRequest &request) const {
  HealthQueryResult query;
  const Runtime runtime = runtimeSnapshot();
  if (runtime.catalog_generation == 0 || !runtime.entity.has_value()) {
    query.status = Status(PDCM_STATUS_NOT_INITIALIZED,
                          "metrics health catalog is not active");
    return query;
  }
  if (request.catalog_generation != 0 &&
      request.catalog_generation != runtime.catalog_generation) {
    query.status =
        Status(PDCM_STATUS_STALE_GENERATION, "health request catalog is stale");
    return query;
  }
  if (request.entity.kind != runtime.entity->kind ||
      request.entity.id != runtime.entity->id) {
    query.status =
        Status(PDCM_STATUS_NOT_FOUND, "health request entity is not present");
    return query;
  }
  if (request.entity != *runtime.entity) {
    query.status =
        Status(PDCM_STATUS_STALE_GENERATION, "health request entity is stale");
    return query;
  }

  const std::int64_t now = monotonicNanoseconds(clock_.monotonicNow());
  if (now < 0) {
    query.status =
        Status(PDCM_STATUS_INVALID_ARGUMENT, "health query time is invalid");
    return query;
  }
  const std::uint64_t freshness =
      effectiveFreshness(runtime.heartbeat.freshness_ns, request.max_age_ns);
  query.aggregate = heartbeatAt(runtime, now, freshness);
  if (request.subsystem_id == kFirmwareHeartbeatHealthId) {
    query.item = query.aggregate;
  } else {
    HealthResult unsupported =
        unknown(runtime, StableHealthCode::kHeartbeatUnsupported,
                ObservationStatus::kUnsupported, now,
                HealthLimitation{HealthLimitationCode::kCatalogGap,
                                 "health subsystem is not supported in P0"});
    unsupported.subsystem_id = request.subsystem_id;
    query.item = std::move(unsupported);
  }
  query.status = request.subsystem_id == kFirmwareHeartbeatHealthId &&
                         query.item.has_value() &&
                         isDeterminateHealth(query.item->state)
                     ? Status::success()
                     : Status(PDCM_STATUS_PARTIAL_RESULT,
                              "health query contains an unknown item");
  return query;
}

std::shared_ptr<const ProcessorGraphSnapshot>
MetricsManager::processorGraph() const noexcept {
  return processor_graph_.snapshot();
}

MetricsManager::Runtime MetricsManager::runtimeSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return runtime_;
}

HealthResult MetricsManager::evaluate(const Runtime &runtime,
                                      const FirmwareHeartbeatEvidence &evidence,
                                      const std::int64_t now_monotonic_ns,
                                      const std::uint64_t freshness_ns) const {
  HealthResult result;
  result.entity = evidence.entity;
  result.subsystem_id = kFirmwareHeartbeatHealthId;
  result.catalog_generation = runtime.catalog_generation;
  result.evaluated_monotonic_time_ns = now_monotonic_ns;
  result.evidence_age_ns =
      now_monotonic_ns - evidence.observed_monotonic_time_ns;
  result.evidence.push_back(evidenceReference(evidence));

  if (evidence.status == ObservationStatus::kValid &&
      static_cast<std::uint64_t>(result.evidence_age_ns) > freshness_ns) {
    result.state = HealthState::kUnknown;
    result.item_status = ObservationStatus::kStale;
    result.code = StableHealthCode::kHeartbeatStale;
    result.limitations.push_back(
        HealthLimitation{HealthLimitationCode::kEvidenceStale,
                         "firmware heartbeat evidence exceeded freshness"});
    return result;
  }
  if (evidence.status == ObservationStatus::kValid) {
    result.item_status = ObservationStatus::kValid;
    switch (evidence.native_class) {
    case HeartbeatNativeClass::kNormal:
      result.state = HealthState::kHealthy;
      result.code = StableHealthCode::kHeartbeatOk;
      return result;
    case HeartbeatNativeClass::kWarning:
      result.state = HealthState::kWarning;
      result.code = StableHealthCode::kHeartbeatWarning;
      return result;
    case HeartbeatNativeClass::kFault:
      result.state = HealthState::kError;
      result.code = StableHealthCode::kHeartbeatFault;
      return result;
    case HeartbeatNativeClass::kUnknownNative:
      break;
    }
  }

  result.state = HealthState::kUnknown;
  result.item_status = evidence.status;
  if (evidence.status == ObservationStatus::kUnsupported) {
    result.code = StableHealthCode::kHeartbeatUnsupported;
    result.limitations.push_back(
        HealthLimitation{HealthLimitationCode::kNativeMappingPending,
                         runtime.heartbeat.limitation});
  } else if (evidence.status == ObservationStatus::kNotAvailable) {
    result.code = StableHealthCode::kHeartbeatMissing;
    result.limitations.push_back(
        HealthLimitation{HealthLimitationCode::kEvidenceMissing,
                         "firmware heartbeat evidence is unavailable"});
  } else if (evidence.error.status.code() == PDCM_STATUS_TIMEOUT) {
    result.code = StableHealthCode::kHeartbeatTimeout;
    result.limitations.push_back(
        HealthLimitation{HealthLimitationCode::kReadFailure,
                         "firmware heartbeat read timed out"});
  } else if (evidence.error.status.code() == PDCM_STATUS_UNAVAILABLE) {
    result.code = StableHealthCode::kProviderUnavailable;
    result.limitations.push_back(HealthLimitation{
        HealthLimitationCode::kProviderUnavailable, "provider is unavailable"});
  } else {
    result.code = StableHealthCode::kHeartbeatReadError;
    result.limitations.push_back(HealthLimitation{
        HealthLimitationCode::kReadFailure, "firmware heartbeat read failed"});
  }
  return result;
}

HealthResult MetricsManager::unknown(const Runtime &runtime,
                                     const StableHealthCode code,
                                     const ObservationStatus item_status,
                                     const std::int64_t now_monotonic_ns,
                                     HealthLimitation limitation) const {
  HealthResult result;
  result.entity = *runtime.entity;
  result.subsystem_id = kFirmwareHeartbeatHealthId;
  result.state = HealthState::kUnknown;
  result.item_status = item_status;
  result.catalog_generation = runtime.catalog_generation;
  result.evaluated_monotonic_time_ns = now_monotonic_ns;
  result.limitations.push_back(std::move(limitation));
  result.code = code;
  return result;
}

HealthResult
MetricsManager::heartbeatAt(const Runtime &runtime,
                            const std::int64_t now_monotonic_ns,
                            const std::uint64_t freshness_ns) const {
  if (!runtime.heartbeat_supported) {
    return unknown(runtime, StableHealthCode::kHeartbeatUnsupported,
                   ObservationStatus::kUnsupported, now_monotonic_ns,
                   HealthLimitation{HealthLimitationCode::kNativeMappingPending,
                                    runtime.heartbeat.limitation});
  }
  const EvidenceReadResult latest = data_manager_.latestHeartbeatEvidence(
      *runtime.entity, runtime.catalog_generation);
  if (!latest.status.ok() || !latest.evidence.has_value()) {
    return unknown(runtime, StableHealthCode::kHeartbeatMissing,
                   ObservationStatus::kNotAvailable, now_monotonic_ns,
                   HealthLimitation{HealthLimitationCode::kEvidenceMissing,
                                    "firmware heartbeat has not been sampled"});
  }
  if (now_monotonic_ns < latest.evidence->observed_monotonic_time_ns) {
    return unknown(runtime, StableHealthCode::kHeartbeatReadError,
                   ObservationStatus::kError, now_monotonic_ns,
                   HealthLimitation{HealthLimitationCode::kReadFailure,
                                    "health query time moved backward"});
  }
  return evaluate(runtime, *latest.evidence, now_monotonic_ns, freshness_ns);
}

} // namespace pdcm
