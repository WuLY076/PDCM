#ifndef PDCM_METRICS_PROCESSOR_HPP_
#define PDCM_METRICS_PROCESSOR_HPP_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "common/observation.hpp"
#include "data/data_manager.hpp"
#include "semantic/catalog_types.hpp"

namespace pdcm {

struct ProcessorMetric {
  MetricId metric;
  std::uint32_t semantic_version{0};

  friend bool operator==(const ProcessorMetric &lhs,
                         const ProcessorMetric &rhs) noexcept {
    return lhs.metric == rhs.metric &&
           lhs.semantic_version == rhs.semantic_version;
  }
};

struct ProcessorContext {
  EntityRef entity;
  std::uint64_t catalog_generation{0};
  std::int64_t evaluated_monotonic_time_ns{0};
  std::uint8_t derivation_depth{0};
};

struct ObservationSnapshot {
  std::vector<Observation> items;
};

struct ProcessorEvaluation {
  Status status;
  std::vector<Observation> outputs;
};

class MetricProcessor {
public:
  virtual ~MetricProcessor() = default;

  [[nodiscard]] virtual std::string id() const = 0;
  [[nodiscard]] virtual std::uint32_t version() const noexcept = 0;
  [[nodiscard]] virtual std::vector<ProcessorMetric> inputs() const = 0;
  [[nodiscard]] virtual std::vector<ProcessorMetric> outputs() const = 0;
  [[nodiscard]] virtual ProcessorEvaluation
  evaluate(const ProcessorContext &context,
           const ObservationSnapshot &snapshot) const = 0;
};

struct ProcessorGraphNode {
  std::shared_ptr<const MetricProcessor> processor;
  std::uint8_t depth{0};
};

struct ProcessorGraphSnapshot {
  std::uint64_t catalog_generation{0};
  std::vector<ProcessorGraphNode> nodes;
};

class ProcessorGraph {
public:
  ProcessorGraph();

  [[nodiscard]] Status rebuild(
      const TargetCatalog &catalog, std::uint64_t catalog_generation,
      const std::vector<std::shared_ptr<const MetricProcessor>> &processors);
  [[nodiscard]] std::shared_ptr<const ProcessorGraphSnapshot>
  snapshot() const noexcept;

private:
  mutable std::mutex writer_mutex_;
  std::shared_ptr<const ProcessorGraphSnapshot> snapshot_;
};

struct ProcessorExecutorLimits {
  std::size_t max_queued_tasks{256};
  std::size_t max_queue_bytes{4U * 1024U * 1024U};
  std::size_t max_recent_keys{4096};

  [[nodiscard]] Status validate() const;
};

struct ProcessorScheduleResult {
  Status status;
  bool scheduled{false};
};

struct ProcessorRunResult {
  Status status;
  bool ran{false};
  bool discarded{false};
  DataCommitResult commit;
};

class ProcessorExecutor {
public:
  explicit ProcessorExecutor(DataManager &data_manager,
                             ProcessorExecutorLimits limits = {});

  ProcessorExecutor(const ProcessorExecutor &) = delete;
  ProcessorExecutor &operator=(const ProcessorExecutor &) = delete;

  [[nodiscard]] ProcessorScheduleResult
  schedule(std::shared_ptr<const MetricProcessor> processor,
           ProcessorContext context, ObservationSnapshot snapshot);
  [[nodiscard]] ProcessorRunResult runNext();
  [[nodiscard]] std::size_t queuedTasks() const;
  [[nodiscard]] std::size_t queuedBytes() const;

private:
  struct Task {
    std::shared_ptr<const MetricProcessor> processor;
    ProcessorContext context;
    ObservationSnapshot snapshot;
    std::string dedup_key;
    std::size_t bytes{0};
  };

  [[nodiscard]] static std::size_t
  taskBytes(const MetricProcessor &processor,
            const ObservationSnapshot &snapshot);
  [[nodiscard]] static std::string
  dedupKey(const MetricProcessor &processor, const ProcessorContext &context,
           const ObservationSnapshot &snapshot);
  void remember(const std::string &key);

  DataManager &data_manager_;
  ProcessorExecutorLimits limits_;
  mutable std::mutex mutex_;
  std::mutex run_mutex_;
  std::deque<Task> queue_;
  std::size_t queued_bytes_{0};
  std::set<std::string> queued_keys_;
  std::deque<std::string> recent_order_;
  std::set<std::string> recent_keys_;
};

} // namespace pdcm

#endif // PDCM_METRICS_PROCESSOR_HPP_
