#ifndef PDCM_METRICS_METRICS_MANAGER_HPP_
#define PDCM_METRICS_METRICS_MANAGER_HPP_

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include "common/clock.hpp"
#include "common/health.hpp"
#include "data/data_manager.hpp"
#include "metrics/processor.hpp"
#include "provider/provider.hpp"
#include "semantic/semantic_catalog.hpp"

namespace pdcm {

struct HealthRequest {
  EntityRef entity;
  std::uint32_t subsystem_id{kFirmwareHeartbeatHealthId};
  std::uint64_t catalog_generation{0};
  std::uint64_t max_age_ns{0};
};

struct HealthQueryResult {
  Status status;
  std::optional<HealthResult> item;
  std::optional<HealthResult> aggregate;
};

class MetricsManager {
public:
  MetricsManager(
      DataManager &data_manager, const Clock &clock,
      std::vector<std::shared_ptr<const MetricProcessor>> processors = {});

  MetricsManager(const MetricsManager &) = delete;
  MetricsManager &operator=(const MetricsManager &) = delete;

  Status activateCatalog(std::shared_ptr<const CatalogView> catalog,
                         const TargetCatalog &target_catalog);
  [[nodiscard]] Status onHeartbeatEvidenceCommitted(std::uint64_t evidence_id);
  [[nodiscard]] Status onProviderStateChanged(ProviderState state);
  [[nodiscard]] Status runFreshnessOnce(MonotonicTime now);
  [[nodiscard]] HealthQueryResult
  queryHealth(const HealthRequest &request) const;
  [[nodiscard]] std::shared_ptr<const ProcessorGraphSnapshot>
  processorGraph() const noexcept;

private:
  struct Runtime {
    std::uint64_t catalog_generation{0};
    std::optional<EntityRef> entity;
    HealthCatalogEntry heartbeat;
    bool heartbeat_supported{false};
  };

  [[nodiscard]] Runtime runtimeSnapshot() const;
  [[nodiscard]] HealthResult evaluate(const Runtime &runtime,
                                      const FirmwareHeartbeatEvidence &evidence,
                                      std::int64_t now_monotonic_ns,
                                      std::uint64_t freshness_ns) const;
  [[nodiscard]] HealthResult unknown(const Runtime &runtime,
                                     StableHealthCode code,
                                     ObservationStatus item_status,
                                     std::int64_t now_monotonic_ns,
                                     HealthLimitation limitation) const;
  [[nodiscard]] HealthResult heartbeatAt(const Runtime &runtime,
                                         std::int64_t now_monotonic_ns,
                                         std::uint64_t freshness_ns) const;

  DataManager &data_manager_;
  const Clock &clock_;
  mutable std::mutex mutex_;
  Runtime runtime_;
  std::vector<std::shared_ptr<const MetricProcessor>> processors_;
  ProcessorGraph processor_graph_;
};

} // namespace pdcm

#endif // PDCM_METRICS_METRICS_MANAGER_HPP_
