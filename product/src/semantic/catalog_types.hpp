#ifndef PDCM_SEMANTIC_CATALOG_TYPES_HPP_
#define PDCM_SEMANTIC_CATALOG_TYPES_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/domain_types.hpp"
#include "common/observation.hpp"
#include "common/status.hpp"
#include "provider/provider.hpp"

namespace pdcm {

enum class EntityState : std::uint8_t {
  kUnknown,
  kReady,
  kError,
};

struct StringAttribute {
  ObservationStatus status{ObservationStatus::kNotAvailable};
  std::string value;

  friend bool operator==(const StringAttribute &lhs,
                         const StringAttribute &rhs) noexcept {
    return lhs.status == rhs.status && lhs.value == rhs.value;
  }
};

struct EntityRecord {
  EntityRef ref;
  EntityState state{EntityState::kUnknown};
  StringAttribute native_id;
  StringAttribute pci_bdf;
  StringAttribute pdrv_version;
  std::string provider_version;
  TargetKind target{TargetKind::kUnknown};
  pdcm_status_t item_status{PDCM_STATUS_SUCCESS};

  friend bool operator==(const EntityRecord &lhs,
                         const EntityRecord &rhs) noexcept {
    return lhs.ref == rhs.ref && lhs.state == rhs.state &&
           lhs.native_id == rhs.native_id && lhs.pci_bdf == rhs.pci_bdf &&
           lhs.pdrv_version == rhs.pdrv_version &&
           lhs.provider_version == rhs.provider_version &&
           lhs.target == rhs.target && lhs.item_status == rhs.item_status;
  }
};

enum class MetricValueKind : std::uint8_t {
  kInt64,
  kUint64,
  kDouble,
  kBool,
  kString,
  kEnum,
};

enum class MetricTemporality : std::uint8_t {
  kGauge,
  kCumulativeCounter,
};

enum class MetricCollectionMode : std::uint8_t {
  kPoll,
  kEvent,
  kDerived,
};

enum class RequirementLevel : std::uint8_t {
  kRequired,
  kConditional,
};

enum class MetricsCatalogStatus : std::uint8_t {
  kBlockedExternal,
  kReady,
};

struct MetricDescriptor {
  MetricId id;
  std::string name;
  MetricValueKind value_type{MetricValueKind::kUint64};
  std::string unit;
  MetricTemporality temporality{MetricTemporality::kGauge};
  EntityKind scope{EntityKind::kDevice};
  MetricCollectionMode collection_mode{MetricCollectionMode::kPoll};
  std::uint64_t default_period_ns{0};
  std::uint64_t min_period_ns{0};
  std::uint64_t freshness_ns{0};
  bool supports_fpga{false};
  bool supports_emu{false};
  RequirementLevel requirement{RequirementLevel::kConditional};
  std::uint32_t semantic_version{0};
  bool provider_mapping_approved{false};
  std::vector<MetricId> dependencies;
};

constexpr std::uint32_t kFirmwareHeartbeatHealthId = 1;

struct HealthCatalogEntry {
  std::uint32_t subsystem_id{kFirmwareHeartbeatHealthId};
  EntityKind scope{EntityKind::kDevice};
  RequirementLevel requirement{RequirementLevel::kRequired};
  std::string evidence_contract{"firmware_heartbeat"};
  std::optional<std::uint32_t> provider_data_id;
  std::uint64_t freshness_ns{0};
  std::uint32_t semantic_version{1};
  std::string limitation{"native mapping pending"};
};

struct TargetCatalog {
  std::uint32_t catalog_version{1};
  TargetKind target{TargetKind::kUnknown};
  MetricsCatalogStatus metrics_status{MetricsCatalogStatus::kBlockedExternal};
  std::vector<MetricDescriptor> metrics;
  std::vector<HealthCatalogEntry> health;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] static TargetCatalog blocked(TargetKind target);
};

enum class CapabilityKind : std::uint8_t {
  kMetricsCatalog,
  kMetric,
  kHealth,
};

enum class CapabilityReason : std::uint8_t {
  kSupported,
  kCatalogBlockedExternal,
  kProviderUnsupported,
  kDependencyMissing,
  kTemporarilyUnavailable,
  kPermissionHidden,
  kPostP0Disabled,
  kTopologyUnsupported,
};

struct CapabilityItem {
  CapabilityKind kind{CapabilityKind::kMetric};
  std::uint32_t id{0};
  bool supported{false};
  CapabilityReason reason{CapabilityReason::kProviderUnsupported};
  std::uint32_t semantic_version{0};
  std::uint64_t catalog_generation{0};

  friend bool operator==(const CapabilityItem &lhs,
                         const CapabilityItem &rhs) noexcept {
    return lhs.kind == rhs.kind && lhs.id == rhs.id &&
           lhs.supported == rhs.supported && lhs.reason == rhs.reason &&
           lhs.semantic_version == rhs.semantic_version &&
           lhs.catalog_generation == rhs.catalog_generation;
  }
};

struct CapabilitySet {
  EntityRef entity;
  std::uint64_t catalog_generation{0};
  std::vector<CapabilityItem> items;
};

struct CatalogDiff {
  std::uint64_t old_generation{0};
  std::uint64_t new_generation{0};
  std::size_t added_entities{0};
  std::size_t removed_entities{0};
  std::size_t changed_entities{0};
  bool capabilities_changed{false};
  bool topology_changed{false};
};

struct CatalogCommitResult {
  Status status;
  std::uint32_t detected_device_count{0};
  std::uint64_t catalog_generation{0};
  bool committed{false};
  CatalogDiff diff;
};
struct EntityListResult {
  Status status;
  std::uint32_t detected_device_count{0};
  std::uint64_t catalog_generation{0};
  std::vector<EntityRecord> entities;
};

struct EntityResolveResult {
  Status status;
  std::optional<EntityRecord> entity;
};

struct CapabilityQueryResult {
  Status status;
  std::optional<CapabilitySet> capabilities;
};

} // namespace pdcm

#endif // PDCM_SEMANTIC_CATALOG_TYPES_HPP_
