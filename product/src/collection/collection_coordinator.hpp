#ifndef PDCM_COLLECTION_COLLECTION_COORDINATOR_HPP_
#define PDCM_COLLECTION_COLLECTION_COORDINATOR_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "collection/watch_manager.hpp"
#include "common/clock.hpp"
#include "common/status.hpp"
#include "data/data_manager.hpp"
#include "provider/provider_manager.hpp"

namespace pdcm {

struct CollectionLimits {
  std::size_t max_plan_jobs{4096};
  std::size_t max_batch_items{64};
  std::size_t max_result_items{4096};
  std::size_t max_result_bytes{1024U * 1024U};

  [[nodiscard]] Status validate() const;
};

struct CollectionPlanItem {
  EffectiveWatchKey key;
  Nanoseconds period{0};
  Nanoseconds freshness{0};
  WatchPriority priority{WatchPriority::kLow};
};

struct CollectionJob {
  std::uint64_t id{0};
  std::string provider_id;
  ProviderDataKind kind{ProviderDataKind::kMetric};
  std::uint32_t isolation_class{0};
  Nanoseconds period{0};
  WatchPriority priority{WatchPriority::kLow};
  std::vector<CollectionPlanItem> items;
};

struct CollectionPlan {
  std::uint64_t version{0};
  std::uint64_t watch_snapshot_version{0};
  std::uint64_t catalog_generation{0};
  MonotonicTime created_at{};
  std::vector<CollectionJob> jobs;
};

struct CollectionRunResult {
  Status status;
  std::uint64_t request_id{0};
  std::uint64_t plan_version{0};
  std::size_t requested_items{0};
  std::size_t normalized_items{0};
  std::size_t contract_violations{0};
  DataCommitResult commit;
};

class CollectionCoordinator {
public:
  CollectionCoordinator(ProviderManager &provider, DataManager &data,
                        std::shared_ptr<const Clock> clock,
                        CollectionLimits limits = {});

  CollectionCoordinator(const CollectionCoordinator &) = delete;
  CollectionCoordinator &operator=(const CollectionCoordinator &) = delete;

  [[nodiscard]] Status
  applyWatchSnapshot(std::shared_ptr<const WatchSnapshot> watches);
  [[nodiscard]] std::shared_ptr<const CollectionPlan> plan() const noexcept;
  [[nodiscard]] CollectionRunResult collectJob(std::uint64_t job_id,
                                               MonotonicTime scheduled_time,
                                               MonotonicTime deadline);

private:
  [[nodiscard]] CollectionRunResult
  normalizeAndCommit(const CollectionPlan &plan, const CollectionJob &job,
                     const ProviderReadRequest &request,
                     const ProviderReadResult &provider_result);

  ProviderManager &provider_;
  DataManager &data_;
  std::shared_ptr<const Clock> clock_;
  CollectionLimits limits_;

  mutable std::mutex plan_writer_mutex_;
  std::shared_ptr<const CollectionPlan> plan_;
  std::atomic<std::uint64_t> next_request_id_{1};
};

} // namespace pdcm

#endif // PDCM_COLLECTION_COLLECTION_COORDINATOR_HPP_
