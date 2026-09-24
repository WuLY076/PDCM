#ifndef PDCM_DATA_DATA_MANAGER_HPP_
#define PDCM_DATA_DATA_MANAGER_HPP_

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "common/clock.hpp"
#include "common/observation.hpp"
#include "semantic/semantic_catalog.hpp"

namespace pdcm {

struct DataKey {
  EntityRef entity;
  MetricId metric;

  friend bool operator==(const DataKey &lhs, const DataKey &rhs) noexcept {
    return lhs.entity == rhs.entity && lhs.metric == rhs.metric;
  }
  friend bool operator<(const DataKey &lhs, const DataKey &rhs) noexcept;
};

struct DataStoreLimits {
  std::size_t shard_count{8};
  std::size_t max_keys{4096};
  std::size_t max_history_samples_per_key{64};
  std::int64_t max_history_duration_ns{UINT64_C(60000000000)};
  std::size_t max_total_bytes{16U * 1024U * 1024U};
  std::size_t max_value_bytes{4096};
  std::size_t max_query_items{4096};
  std::size_t max_tombstones{1024};

  [[nodiscard]] Status validate() const;
};

struct DataCommitResult {
  Status status;
  std::uint64_t commit_epoch{0};
  std::size_t committed_items{0};
};

struct DataReadResult {
  Status status;
  std::uint64_t catalog_generation{0};
  std::uint64_t observed_commit_epoch{0};
  std::vector<Observation> items;
};

struct DataHistoryResult {
  Status status;
  std::vector<Observation> samples;
};

struct EntityTombstone {
  EntityRef entity;
  std::uint64_t retired_catalog_generation{0};
};

class DataManager {
public:
  explicit DataManager(DataStoreLimits limits = {});

  DataManager(const DataManager &) = delete;
  DataManager &operator=(const DataManager &) = delete;

  Status activateCatalog(std::shared_ptr<const CatalogView> catalog,
                         const TargetCatalog &target_catalog);
  [[nodiscard]] DataCommitResult
  commit(const std::vector<Observation> &observations);
  [[nodiscard]] DataReadResult
  readLatest(std::vector<DataKey> keys, bool allow_stale,
             std::int64_t now_monotonic_ns,
             std::uint64_t required_catalog_generation = 0) const;
  [[nodiscard]] DataHistoryResult history(const DataKey &key) const;
  [[nodiscard]] std::optional<MetricDescriptor>
  metricDescriptor(MetricId metric) const;
  [[nodiscard]] Status waitForEpoch(std::uint64_t epoch,
                                    MonotonicTime deadline) const;

  [[nodiscard]] std::uint64_t catalogGeneration() const;
  [[nodiscard]] std::uint64_t commitEpoch() const;
  [[nodiscard]] std::size_t keyCount() const;
  [[nodiscard]] std::size_t storedBytes() const;
  [[nodiscard]] std::vector<EntityTombstone> tombstones() const;

private:
  struct Entry {
    std::optional<Observation> latest;
    std::optional<Observation> last_valid;
    std::deque<Observation> history;
  };

  struct Shard {
    mutable std::mutex mutex;
    std::map<DataKey, Entry> entries;
  };

  struct CatalogState {
    std::uint64_t generation{0};
    bool topology_unsupported{false};
    std::vector<EntityRef> entities;
    std::map<std::uint32_t, MetricDescriptor> metrics;
  };

  [[nodiscard]] std::size_t shardIndex(const DataKey &key) const noexcept;
  [[nodiscard]] static bool valueMatches(const MetricDescriptor &descriptor,
                                         const MetricValue &value);
  [[nodiscard]] static std::size_t
  observationBytes(const Observation &observation);
  [[nodiscard]] static std::size_t entryBytes(const Entry &entry);
  [[nodiscard]] static Observation
  unavailable(const DataKey &key, const MetricDescriptor &descriptor,
              std::uint64_t catalog_generation, std::uint64_t commit_epoch,
              std::int64_t now_monotonic_ns, pdcm_status_t status,
              ObservationStatus observation_status, const char *message);

  DataStoreLimits limits_;
  std::vector<std::unique_ptr<Shard>> shards_;

  mutable std::mutex state_mutex_;
  mutable std::condition_variable state_changed_;
  CatalogState catalog_;
  std::uint64_t commit_epoch_{0};
  std::deque<EntityTombstone> tombstones_;
};

} // namespace pdcm

#endif // PDCM_DATA_DATA_MANAGER_HPP_
