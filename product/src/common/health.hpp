#ifndef PDCM_COMMON_HEALTH_HPP_
#define PDCM_COMMON_HEALTH_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/observation.hpp"

namespace pdcm {

enum class HeartbeatNativeClass : std::uint8_t {
  kNormal,
  kWarning,
  kFault,
  kUnknownNative,
};

enum class HealthState : std::uint8_t {
  kHealthy,
  kUnknown,
  kWarning,
  kError,
};

enum class StableHealthCode : std::uint8_t {
  kHeartbeatOk,
  kHeartbeatWarning,
  kHeartbeatFault,
  kHeartbeatMissing,
  kHeartbeatStale,
  kHeartbeatTimeout,
  kHeartbeatReadError,
  kHeartbeatUnsupported,
  kProviderUnavailable,
  kProcessorError,
};

enum class HealthLimitationCode : std::uint8_t {
  kCatalogGap,
  kNativeMappingPending,
  kProviderUnavailable,
  kEvidenceMissing,
  kEvidenceStale,
  kReadFailure,
  kProcessorError,
};

struct HealthLimitation {
  HealthLimitationCode code{HealthLimitationCode::kEvidenceMissing};
  std::string detail;

  friend bool operator==(const HealthLimitation &lhs,
                         const HealthLimitation &rhs) noexcept {
    return lhs.code == rhs.code && lhs.detail == rhs.detail;
  }
};

struct FirmwareHeartbeatEvidence {
  std::uint64_t evidence_id{0};
  EntityRef entity;
  std::uint32_t provider_data_id{0};
  HeartbeatNativeClass native_class{HeartbeatNativeClass::kUnknownNative};
  ObservationStatus status{ObservationStatus::kNotAvailable};
  std::uint64_t sequence_or_token{0};
  std::optional<std::int64_t> source_sample_time_ns;
  std::int64_t observed_monotonic_time_ns{0};
  SourceMetadata source;
  ErrorMetadata error;
  std::uint64_t catalog_generation{0};
};

struct EvidenceRef {
  std::uint64_t evidence_id{0};
  std::uint64_t sequence_or_token{0};
  std::optional<std::int64_t> source_sample_time_ns;
  std::int64_t observed_monotonic_time_ns{0};
  SourceMetadata source;

  friend bool operator==(const EvidenceRef &lhs,
                         const EvidenceRef &rhs) noexcept {
    return lhs.evidence_id == rhs.evidence_id &&
           lhs.sequence_or_token == rhs.sequence_or_token &&
           lhs.source_sample_time_ns == rhs.source_sample_time_ns &&
           lhs.observed_monotonic_time_ns == rhs.observed_monotonic_time_ns &&
           lhs.source.provider == rhs.source.provider &&
           lhs.source.native_source == rhs.source.native_source;
  }
};

struct HealthResult {
  EntityRef entity;
  std::uint32_t subsystem_id{0};
  HealthState state{HealthState::kUnknown};
  ObservationStatus item_status{ObservationStatus::kNotAvailable};
  std::uint64_t catalog_generation{0};
  std::int64_t evaluated_monotonic_time_ns{0};
  std::int64_t evidence_age_ns{0};
  std::vector<EvidenceRef> evidence;
  std::vector<HealthLimitation> limitations;
  StableHealthCode code{StableHealthCode::kHeartbeatMissing};
};

[[nodiscard]] Status
validateFirmwareHeartbeatEvidence(const FirmwareHeartbeatEvidence &evidence);
[[nodiscard]] Status validateHealthResult(const HealthResult &result);
[[nodiscard]] bool isDeterminateHealth(HealthState state) noexcept;

} // namespace pdcm

#endif // PDCM_COMMON_HEALTH_HPP_
