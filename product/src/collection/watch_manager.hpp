#ifndef PDCM_COLLECTION_WATCH_MANAGER_HPP_
#define PDCM_COLLECTION_WATCH_MANAGER_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "common/status.hpp"
#include "provider/provider.hpp"
#include "semantic/semantic_catalog.hpp"

namespace pdcm {

using WatchId = std::uint64_t;

enum class WatchOwnerKind : std::uint8_t {
  kSession,
  kFirmwareHeartbeat,
  kSystemBaseline,
};

struct WatchOwner {
  WatchOwnerKind kind{WatchOwnerKind::kSession};
  std::uint64_t id{0};

  friend bool operator==(const WatchOwner &lhs,
                         const WatchOwner &rhs) noexcept {
    return lhs.kind == rhs.kind && lhs.id == rhs.id;
  }
  friend bool operator<(const WatchOwner &lhs, const WatchOwner &rhs) noexcept;
};

enum class WatchPriority : std::uint8_t {
  kLow,
  kNormal,
  kHigh,
  kHeartbeat,
};

enum class WatchDeliveryMode : std::uint8_t {
  kCacheOnly,
  kEvent,
};

struct WatchRequirement {
  std::uint64_t catalog_generation{0};
  EntityRef entity;
  std::vector<MetricId> metrics;
  Nanoseconds period{0};
  Nanoseconds freshness{0};
  Nanoseconds retention{0};
  std::uint64_t sample_limit{0};
  WatchPriority priority{WatchPriority::kNormal};
  WatchDeliveryMode delivery{WatchDeliveryMode::kCacheOnly};
  bool allow_partial{false};
};

struct LogicalWatch {
  WatchId id{0};
  WatchOwner owner;
  WatchRequirement requirement;
};

struct EffectiveWatchKey {
  std::string provider_id;
  EntityRef entity;
  ProviderDataKind kind{ProviderDataKind::kMetric};
  std::uint32_t data_id{0};
  std::uint32_t isolation_class{0};

  friend bool operator==(const EffectiveWatchKey &lhs,
                         const EffectiveWatchKey &rhs) noexcept;
  friend bool operator<(const EffectiveWatchKey &lhs,
                        const EffectiveWatchKey &rhs) noexcept;
};

struct EffectiveWatch {
  EffectiveWatchKey key;
  Nanoseconds period{0};
  Nanoseconds freshness{0};
  Nanoseconds retention{0};
  WatchPriority priority{WatchPriority::kLow};
  std::vector<WatchId> logical_watches;
};

struct WatchSnapshot {
  std::uint64_t version{0};
  std::uint64_t catalog_generation{0};
  std::vector<LogicalWatch> logical_watches;
  std::vector<EffectiveWatch> effective_watches;
};

struct WatchLimits {
  std::size_t max_logical_watches{4096};
  std::size_t max_effective_watches{4096};
  std::size_t max_metrics_per_watch{256};

  [[nodiscard]] Status validate() const;
};

struct WatchCreateResult {
  Status status;
  WatchId watch_id{0};
  std::vector<MetricId> unsupported_metrics;
};

class WatchManager {
public:
  explicit WatchManager(WatchLimits limits = {});

  WatchManager(const WatchManager &) = delete;
  WatchManager &operator=(const WatchManager &) = delete;

  [[nodiscard]] Status
  activateCatalog(std::shared_ptr<const CatalogView> catalog,
                  const TargetCatalog &target_catalog);
  [[nodiscard]] WatchCreateResult create(const WatchOwner &owner,
                                         WatchRequirement requirement);
  [[nodiscard]] Status destroy(const WatchOwner &owner, WatchId watch_id);
  [[nodiscard]] Status removeOwner(const WatchOwner &owner);
  [[nodiscard]] Status recordSamples(std::vector<WatchId> watch_ids,
                                     MonotonicTime first_scheduled_time,
                                     Nanoseconds period,
                                     std::uint64_t sample_count = 1);
  [[nodiscard]] std::shared_ptr<const WatchSnapshot> snapshot() const noexcept;

private:
  struct CatalogState {
    std::shared_ptr<const CatalogView> view;
    std::map<std::uint32_t, MetricDescriptor> descriptors;
    std::map<std::uint32_t, bool> supported;
    std::string provider_id;
    std::optional<EntityRef> entity;
    bool topology_unsupported{false};
  };

  [[nodiscard]] Status
  validateRequirement(const WatchRequirement &requirement,
                      std::vector<MetricId> &supported_metrics,
                      std::vector<MetricId> &unsupported_metrics) const;
  [[nodiscard]] std::shared_ptr<const WatchSnapshot>
  buildSnapshot(const std::map<WatchId, LogicalWatch> &logical,
                std::uint64_t version) const;
  [[nodiscard]] Status publish(std::map<WatchId, LogicalWatch> next);

  WatchLimits limits_;
  mutable std::mutex mutex_;
  CatalogState catalog_;
  std::map<WatchId, LogicalWatch> logical_;
  std::shared_ptr<const WatchSnapshot> snapshot_;
  std::map<WatchId, std::uint64_t> sample_counts_;
  std::map<WatchId, MonotonicTime> last_sample_times_;
  WatchId next_watch_id_{1};
};

} // namespace pdcm

#endif // PDCM_COLLECTION_WATCH_MANAGER_HPP_
