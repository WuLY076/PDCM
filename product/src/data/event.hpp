#ifndef PDCM_DATA_EVENT_HPP_
#define PDCM_DATA_EVENT_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/domain_types.hpp"
#include "common/health.hpp"
#include "common/observation.hpp"
#include "semantic/catalog_types.hpp"

namespace pdcm {

enum class EventType : std::uint8_t {
  kCatalogChanged,
  kEntityChanged,
  kCapabilityChanged,
  kMetricUpdate,
  kFirmwareHeartbeatHealthChanged,
  kProviderStateChanged,
  kSubscriptionLossMarker,
  kDaemonDraining,
};

enum class EventSeverity : std::uint8_t {
  kInfo,
  kWarning,
  kError,
  kCritical,
};

struct MetricUpdatePayload {
  ObservationStatus status{ObservationStatus::kNotAvailable};
  std::uint64_t commit_epoch{0};
};

struct SubscriptionLossPayload {
  std::uint64_t first_dropped_sequence{0};
  std::uint64_t last_dropped_sequence{0};
  std::uint64_t dropped_count{0};
};

struct HealthChangePayload {
  HealthState previous_state{HealthState::kUnknown};
  HealthState state{HealthState::kUnknown};
  StableHealthCode previous_code{StableHealthCode::kHeartbeatMissing};
  StableHealthCode code{StableHealthCode::kHeartbeatMissing};
  std::vector<EvidenceRef> evidence;
  std::int64_t evidence_age_ns{0};
};

using EventPayload =
    std::variant<std::monostate, MetricUpdatePayload, SubscriptionLossPayload,
                 HealthChangePayload, std::string>;

struct EventDraft {
  EventType type{EventType::kCatalogChanged};
  EventSeverity severity{EventSeverity::kInfo};
  std::optional<EntityRef> entity;
  std::optional<MetricId> metric;
  std::optional<std::uint32_t> health_subsystem;
  std::int64_t occurrence_time_ns{0};
  std::uint64_t catalog_generation{0};
  EventPayload payload;
};

struct PdcmEvent {
  std::uint64_t boot_epoch{0};
  std::uint64_t sequence{0};
  EventType type{EventType::kCatalogChanged};
  EventSeverity severity{EventSeverity::kInfo};
  std::optional<EntityRef> entity;
  std::optional<MetricId> metric;
  std::optional<std::uint32_t> health_subsystem;
  std::int64_t occurrence_time_ns{0};
  std::uint64_t catalog_generation{0};
  EventPayload payload;
};

[[nodiscard]] bool isNonDroppable(EventType type) noexcept;
[[nodiscard]] bool isStateChange(EventType type) noexcept;

} // namespace pdcm

#endif // PDCM_DATA_EVENT_HPP_
