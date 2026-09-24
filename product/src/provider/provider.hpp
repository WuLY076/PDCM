#ifndef PDCM_PROVIDER_PROVIDER_HPP_
#define PDCM_PROVIDER_PROVIDER_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/clock.hpp"
#include "common/domain_types.hpp"
#include "common/observation.hpp"
#include "common/status.hpp"

namespace pdcm {

enum class ProviderState : std::uint8_t {
  kUninitialized,
  kReady,
  kUnavailable,
  kShutdown,
};

enum class ProviderFailurePhase : std::uint8_t {
  kNone,
  kLoad,
  kEntry,
  kAbi,
  kNativeInitialize,
  kDiscovery,
};

enum class TargetKind : std::uint8_t {
  kUnknown,
  kFpga,
  kEmu,
};

enum class ProviderEntityState : std::uint8_t {
  kUnknown,
  kReady,
  kLost,
  kError,
};

enum class ProviderDataKind : std::uint8_t {
  kMetric,
  kHeartbeatEvidence,
};

struct ProviderConcurrency {
  std::size_t max_concurrency{1};
  bool reentrant{false};
};

struct ProviderInitOptions {
  TargetKind target{TargetKind::kUnknown};
  MonotonicTime deadline{};
};

struct ProviderEntity {
  std::string stable_native_id;
  std::string pci_bdf;
  std::string pdrv_version;
  std::string incarnation;
  ProviderEntityState state{ProviderEntityState::kUnknown};
  bool manageable{false};
};

struct ProviderCapability {
  ProviderDataKind kind{ProviderDataKind::kMetric};
  std::uint32_t data_id{0};
  bool supported{false};
  std::string reason;
};

struct ProviderDescriptor {
  std::string provider_version;
  TargetKind target{TargetKind::kUnknown};
  ProviderState state{ProviderState::kUninitialized};
  ProviderFailurePhase failure_phase{ProviderFailurePhase::kNone};
  std::vector<ProviderEntity> entities;
  std::vector<ProviderCapability> capabilities;
  std::vector<Status> errors;
  std::uint32_t detected_device_count{0};
};

struct ProviderReadItem {
  EntityRef entity;
  ProviderDataKind kind{ProviderDataKind::kMetric};
  std::uint32_t data_id{0};

  friend bool operator==(const ProviderReadItem &lhs,
                         const ProviderReadItem &rhs) noexcept {
    return lhs.entity == rhs.entity && lhs.kind == rhs.kind &&
           lhs.data_id == rhs.data_id;
  }
};

struct ProviderReadRequest {
  std::uint64_t request_id{0};
  std::uint64_t plan_generation{0};
  std::uint64_t catalog_generation{0};
  MonotonicTime scheduled_time{};
  MonotonicTime deadline{};
  std::vector<ProviderReadItem> items;
};

struct ProviderReadItemResult {
  ProviderReadItem item;
  ObservationStatus status{ObservationStatus::kNotAvailable};
  std::optional<MetricValue> value;
  std::optional<std::int64_t> source_sample_time_ns;
  std::string native_source;
  std::int64_t native_code{0};
  bool retryable{false};
};

struct ProviderReadResult {
  Status call_status;
  std::vector<ProviderReadItemResult> items;
};

struct ProviderDiscoveryResult {
  Status call_status;
  ProviderDescriptor descriptor;
};

class Provider {
public:
  virtual ~Provider() = default;

  virtual Status initialize(const ProviderInitOptions &options) = 0;
  virtual void shutdown() noexcept = 0;

  [[nodiscard]] virtual ProviderState state() const noexcept = 0;
  [[nodiscard]] virtual ProviderConcurrency concurrency() const noexcept = 0;

  virtual ProviderDiscoveryResult discover(MonotonicTime deadline) = 0;
  virtual ProviderReadResult batchRead(const ProviderReadRequest &request) = 0;
};

} // namespace pdcm

#endif // PDCM_PROVIDER_PROVIDER_HPP_
