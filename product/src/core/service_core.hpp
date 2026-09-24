#ifndef PDCM_CORE_SERVICE_CORE_HPP_
#define PDCM_CORE_SERVICE_CORE_HPP_

#include <cstdint>
#include <memory>
#include <mutex>

#include "common/clock.hpp"
#include "common/status.hpp"
#include "core/runtime_config.hpp"
#include "provider/provider.hpp"
#include "provider/provider_manager.hpp"
#include "semantic/semantic_catalog.hpp"

namespace pdcm {

enum class CoreState : std::uint8_t {
  kCreated,
  kStarting,
  kReady,
  kDegraded,
  kFailed,
  kStopping,
  kStopped,
};

enum class CoreDegradedReason : std::uint8_t {
  kNone,
  kProviderUnavailable,
  kDiscoveryFailed,
  kTopologyUnsupported,
};

struct CoreSnapshot {
  CoreState state{CoreState::kCreated};
  ProviderState provider_state{ProviderState::kUninitialized};
  CoreDegradedReason degraded_reason{CoreDegradedReason::kNone};
  pdcm_status_t detail_status{PDCM_STATUS_SUCCESS};
  std::uint32_t detected_device_count{0};
  std::uint64_t catalog_generation{0};
};

class PdcmServiceCore {
public:
  PdcmServiceCore(RuntimeConfig config, std::unique_ptr<Provider> provider,
                  std::shared_ptr<const Clock> clock);
  PdcmServiceCore(RuntimeConfig config, std::unique_ptr<Provider> provider,
                  std::shared_ptr<const Clock> clock,
                  TargetCatalog target_catalog);
  ~PdcmServiceCore();

  PdcmServiceCore(const PdcmServiceCore &) = delete;
  PdcmServiceCore &operator=(const PdcmServiceCore &) = delete;

  Status start();
  Status stop() noexcept;

  [[nodiscard]] CoreSnapshot snapshot() const;
  [[nodiscard]] const RuntimeConfig &config() const noexcept;
  [[nodiscard]] std::shared_ptr<const CatalogView>
  catalogSnapshot() const noexcept;

private:
  void publish(CoreState state, ProviderState provider_state,
               CoreDegradedReason degraded_reason, pdcm_status_t detail_status,
               std::uint32_t detected_device_count,
               std::uint64_t catalog_generation);

  RuntimeConfig config_;
  ProviderManager provider_manager_;
  std::shared_ptr<const Clock> clock_;

  mutable std::mutex state_mutex_;
  SemanticCatalog semantic_catalog_;
  CoreSnapshot snapshot_;
};

} // namespace pdcm

#endif // PDCM_CORE_SERVICE_CORE_HPP_
