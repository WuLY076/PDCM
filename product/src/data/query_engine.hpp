#ifndef PDCM_DATA_QUERY_ENGINE_HPP_
#define PDCM_DATA_QUERY_ENGINE_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "common/clock.hpp"
#include "common/status.hpp"
#include "data/data_manager.hpp"

namespace pdcm {

enum class ReadPolicy : std::uint8_t {
  kCacheOnly,
  kCacheOrFresh,
  kFreshRequired,
};

enum class QueryConsistency : std::uint8_t {
  kBestEffort,
  kAtLeastEpoch,
  kSameBatch,
};

struct QueryLimits {
  std::size_t max_items{4096};
  std::size_t max_result_bytes{4U * 1024U * 1024U};

  [[nodiscard]] Status validate() const;
};

struct QueryRequest {
  std::vector<DataKey> selection;
  std::uint64_t max_age_ns{0};
  ReadPolicy read_policy{ReadPolicy::kCacheOnly};
  QueryConsistency consistency{QueryConsistency::kBestEffort};
  bool allow_stale{false};
  std::uint64_t catalog_generation{0};
  std::uint64_t required_epoch{0};
  std::size_t max_result_items{4096};
  std::size_t max_result_bytes{4U * 1024U * 1024U};
  MonotonicTime deadline{MonotonicTime::max()};
};

struct QueryResult {
  Status status;
  std::uint64_t catalog_generation{0};
  std::uint64_t observed_commit_epoch{0};
  std::size_t required_items{0};
  std::size_t required_bytes{0};
  bool truncated{false};
  std::vector<Observation> items;
};

class FreshReadCoordinator {
public:
  virtual ~FreshReadCoordinator() = default;

  [[nodiscard]] virtual Status freshRead(const std::vector<DataKey> &keys,
                                         std::uint64_t catalog_generation,
                                         MonotonicTime deadline) = 0;
};

class QueryEngine {
public:
  QueryEngine(DataManager &data_manager, std::shared_ptr<const Clock> clock,
              FreshReadCoordinator *fresh_reads = nullptr,
              QueryLimits limits = {});

  QueryEngine(const QueryEngine &) = delete;
  QueryEngine &operator=(const QueryEngine &) = delete;

  [[nodiscard]] QueryResult execute(const QueryRequest &request) const;

private:
  [[nodiscard]] Status validate(const QueryRequest &request) const;
  [[nodiscard]] std::vector<DataKey>
  keysNeedingFreshRead(const QueryRequest &request,
                       const DataReadResult &snapshot,
                       std::int64_t now_monotonic_ns) const;
  [[nodiscard]] static std::size_t itemBytes(const Observation &item);
  [[nodiscard]] QueryResult finalize(const QueryRequest &request,
                                     DataReadResult snapshot,
                                     const Status &fresh_status) const;

  DataManager &data_manager_;
  std::shared_ptr<const Clock> clock_;
  FreshReadCoordinator *fresh_reads_;
  QueryLimits limits_;
};

} // namespace pdcm

#endif // PDCM_DATA_QUERY_ENGINE_HPP_
