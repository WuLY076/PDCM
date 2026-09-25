#include "common/health.hpp"

#include "semantic/catalog_types.hpp"

namespace pdcm {
namespace {

bool evidenceRefValid(const EvidenceRef &evidence) {
  return evidence.evidence_id != 0 &&
         evidence.observed_monotonic_time_ns >= 0 &&
         (!evidence.source_sample_time_ns.has_value() ||
          *evidence.source_sample_time_ns >= 0) &&
         !evidence.source.provider.empty();
}

bool codeMatchesState(const HealthState state, const StableHealthCode code) {
  switch (state) {
  case HealthState::kHealthy:
    return code == StableHealthCode::kHeartbeatOk;
  case HealthState::kWarning:
    return code == StableHealthCode::kHeartbeatWarning;
  case HealthState::kError:
    return code == StableHealthCode::kHeartbeatFault;
  case HealthState::kUnknown:
    return code != StableHealthCode::kHeartbeatOk &&
           code != StableHealthCode::kHeartbeatWarning &&
           code != StableHealthCode::kHeartbeatFault;
  }
  return false;
}

} // namespace

bool isDeterminateHealth(const HealthState state) noexcept {
  return state == HealthState::kHealthy || state == HealthState::kWarning ||
         state == HealthState::kError;
}

Status
validateFirmwareHeartbeatEvidence(const FirmwareHeartbeatEvidence &evidence) {
  if (evidence.entity.kind != EntityKind::kDevice ||
      evidence.entity.generation == 0 || evidence.provider_data_id == 0 ||
      evidence.catalog_generation == 0 ||
      evidence.observed_monotonic_time_ns < 0 ||
      (evidence.source_sample_time_ns.has_value() &&
       *evidence.source_sample_time_ns < 0) ||
      evidence.source.provider.empty()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "heartbeat evidence identity or time is invalid");
  }
  if (evidence.status == ObservationStatus::kStale) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "stale is not a physical heartbeat evidence status");
  }

  if (evidence.status == ObservationStatus::kValid) {
    if (evidence.native_class == HeartbeatNativeClass::kUnknownNative ||
        !evidence.source_sample_time_ns.has_value() ||
        !evidence.error.status.ok()) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "valid heartbeat evidence is incomplete");
    }
  } else if (evidence.native_class != HeartbeatNativeClass::kUnknownNative ||
             evidence.error.status.ok()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "failed heartbeat evidence metadata is inconsistent");
  }
  return Status::success();
}

Status validateHealthResult(const HealthResult &result) {
  if (result.entity.kind != EntityKind::kDevice ||
      result.entity.generation == 0 ||
      result.subsystem_id != kFirmwareHeartbeatHealthId ||
      result.catalog_generation == 0 ||
      result.evaluated_monotonic_time_ns < 0 || result.evidence_age_ns < 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "health result identity or time is invalid");
  }
  for (const EvidenceRef &evidence : result.evidence) {
    if (!evidenceRefValid(evidence)) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "health evidence reference is invalid");
    }
  }
  for (const HealthLimitation &limitation : result.limitations) {
    if (limitation.detail.empty()) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "health limitation detail is empty");
    }
  }
  if (!codeMatchesState(result.state, result.code)) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "health state and stable code disagree");
  }

  if (isDeterminateHealth(result.state)) {
    if (result.item_status != ObservationStatus::kValid ||
        result.evidence.empty()) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "determinate health requires valid evidence");
    }
  } else if (result.item_status == ObservationStatus::kValid) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "unknown health cannot have a valid item status");
  }
  return Status::success();
}

} // namespace pdcm
