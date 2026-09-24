#include "data/query_engine.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace pdcm {
namespace {

bool isReadableStatus(const Status &status) {
  return status.ok() || status.code() == PDCM_STATUS_PARTIAL_RESULT;
}

bool allItemsAtEpoch(const DataReadResult &snapshot,
                     const std::uint64_t required_epoch) {
  return std::all_of(snapshot.items.begin(), snapshot.items.end(),
                     [required_epoch](const Observation &item) {
                       return item.commit_epoch >= required_epoch;
                     });
}

} // namespace

Status QueryLimits::validate() const {
  if (max_items == 0 || max_result_bytes == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "query engine limits must be non-zero");
  }
  return Status::success();
}

QueryEngine::QueryEngine(DataManager &data_manager,
                         std::shared_ptr<const Clock> clock,
                         FreshReadCoordinator *fresh_reads, QueryLimits limits)
    : data_manager_(data_manager), clock_(std::move(clock)),
      fresh_reads_(fresh_reads), limits_(limits) {}

Status QueryEngine::validate(const QueryRequest &request) const {
  const Status limits_status = limits_.validate();
  if (!limits_status.ok()) {
    return limits_status;
  }
  if (clock_ == nullptr || request.selection.empty() ||
      request.selection.size() > limits_.max_items ||
      request.max_result_items == 0 ||
      request.max_result_items > limits_.max_items ||
      request.max_result_bytes == 0 ||
      request.max_result_bytes > limits_.max_result_bytes) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "query request or engine dependencies are invalid");
  }

  switch (request.read_policy) {
  case ReadPolicy::kCacheOnly:
  case ReadPolicy::kCacheOrFresh:
  case ReadPolicy::kFreshRequired:
    break;
  default:
    return Status(PDCM_STATUS_INVALID_ARGUMENT, "unknown query read policy");
  }
  switch (request.consistency) {
  case QueryConsistency::kBestEffort:
  case QueryConsistency::kAtLeastEpoch:
  case QueryConsistency::kSameBatch:
    break;
  default:
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "unknown query consistency policy");
  }

  std::vector<DataKey> keys = request.selection;
  std::sort(keys.begin(), keys.end());
  if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "query selection contains duplicates");
  }
  if (request.consistency == QueryConsistency::kAtLeastEpoch &&
      request.required_epoch == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "at-least-epoch query requires an epoch");
  }
  if (request.consistency != QueryConsistency::kAtLeastEpoch &&
      request.required_epoch != 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "required epoch is only valid for at-least-epoch queries");
  }
  return Status::success();
}

std::vector<DataKey>
QueryEngine::keysNeedingFreshRead(const QueryRequest &request,
                                  const DataReadResult &snapshot,
                                  const std::int64_t now_monotonic_ns) const {
  if (request.read_policy == ReadPolicy::kCacheOnly) {
    return {};
  }
  std::vector<DataKey> keys;
  for (const Observation &item : snapshot.items) {
    const DataKey key{item.entity, item.metric};
    if (item.status == ObservationStatus::kUnsupported) {
      continue;
    }
    if (request.read_policy == ReadPolicy::kFreshRequired) {
      keys.push_back(key);
      continue;
    }
    if (item.status != ObservationStatus::kValid) {
      keys.push_back(key);
      continue;
    }

    const std::optional<MetricDescriptor> descriptor =
        data_manager_.metricDescriptor(item.metric);
    if (!descriptor.has_value()) {
      continue;
    }
    std::uint64_t effective_age = descriptor->freshness_ns;
    if (request.max_age_ns != 0 &&
        (effective_age == 0 || request.max_age_ns < effective_age)) {
      effective_age = request.max_age_ns;
    }
    if (effective_age == 0) {
      continue;
    }
    if (now_monotonic_ns < item.observed_monotonic_time_ns ||
        static_cast<std::uint64_t>(now_monotonic_ns -
                                   item.observed_monotonic_time_ns) >
            effective_age) {
      keys.push_back(key);
    }
  }
  return keys;
}

std::size_t QueryEngine::itemBytes(const Observation &item) {
  std::size_t bytes =
      sizeof(Observation) + item.source.provider.size() +
      item.source.native_source.size() + item.error.status.message().size() +
      item.derivation.processor_id.size() +
      item.derivation.input_sequences.size() * sizeof(std::uint64_t);
  if (item.latest_failure.has_value()) {
    bytes +=
        sizeof(ErrorMetadata) + item.latest_failure->status.message().size();
  }
  if (item.value.has_value()) {
    bytes += std::visit(
        [](const auto &value) -> std::size_t {
          using Value = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, std::string>) {
            return value.size();
          } else if constexpr (std::is_same_v<Value,
                                              std::vector<std::uint8_t>>) {
            return value.size();
          } else {
            return sizeof(Value);
          }
        },
        *item.value);
  }
  return bytes;
}

QueryResult QueryEngine::finalize(const QueryRequest &request,
                                  DataReadResult snapshot,
                                  const Status &fresh_status) const {
  QueryResult result;
  result.status = snapshot.status;
  result.catalog_generation = snapshot.catalog_generation;
  result.observed_commit_epoch = snapshot.observed_commit_epoch;
  result.required_items = snapshot.items.size();

  if (!isReadableStatus(snapshot.status)) {
    return result;
  }

  if (request.consistency == QueryConsistency::kSameBatch &&
      !snapshot.items.empty()) {
    const std::uint64_t epoch = snapshot.items.front().commit_epoch;
    const bool same_batch =
        std::all_of(snapshot.items.begin(), snapshot.items.end(),
                    [epoch](const Observation &item) {
                      return item.commit_epoch == epoch;
                    });
    if (!same_batch) {
      result.status = Status(PDCM_STATUS_UNAVAILABLE,
                             "query items do not share one commit epoch");
      return result;
    }
  }

  for (const Observation &item : snapshot.items) {
    const std::size_t bytes = itemBytes(item);
    if (result.required_bytes >
        std::numeric_limits<std::size_t>::max() - bytes) {
      result.status =
          Status(PDCM_STATUS_RESOURCE_EXHAUSTED, "query size overflow");
      return result;
    }
    result.required_bytes += bytes;
  }

  std::size_t returned_bytes = 0;
  for (Observation &item : snapshot.items) {
    const std::size_t bytes = itemBytes(item);
    if (result.items.size() == request.max_result_items ||
        bytes > request.max_result_bytes - returned_bytes) {
      result.truncated = true;
      break;
    }
    returned_bytes += bytes;
    result.items.push_back(std::move(item));
  }

  if (result.items.size() != result.required_items) {
    result.truncated = true;
  }
  if (result.truncated) {
    result.status = Status(PDCM_STATUS_BUFFER_TOO_SMALL,
                           "query result was deterministically truncated");
  } else if (!fresh_status.ok() &&
             fresh_status.code() != PDCM_STATUS_PARTIAL_RESULT) {
    result.status = fresh_status;
  }
  return result;
}

QueryResult QueryEngine::execute(const QueryRequest &request) const {
  const Status request_status = validate(request);
  if (!request_status.ok()) {
    QueryResult result;
    result.status = request_status;
    return result;
  }

  if (request.consistency == QueryConsistency::kAtLeastEpoch) {
    const Status wait_status =
        data_manager_.waitForEpoch(request.required_epoch, request.deadline);
    if (!wait_status.ok()) {
      QueryResult result;
      result.status = wait_status;
      return result;
    }
  }

  std::int64_t now_ns = clock_->monotonicNow().time_since_epoch().count();
  DataReadResult snapshot =
      data_manager_.readLatest(request.selection, request.allow_stale, now_ns,
                               request.catalog_generation);
  if (!isReadableStatus(snapshot.status)) {
    return finalize(request, std::move(snapshot), Status::success());
  }

  Status fresh_status = Status::success();
  const std::vector<DataKey> fresh_keys =
      keysNeedingFreshRead(request, snapshot, now_ns);
  if (!fresh_keys.empty()) {
    if (fresh_reads_ == nullptr) {
      fresh_status =
          Status(PDCM_STATUS_UNAVAILABLE, "fresh read coordinator unavailable");
    } else if (clock_->monotonicNow() >= request.deadline) {
      fresh_status = Status(PDCM_STATUS_TIMEOUT, "fresh read deadline expired");
    } else {
      fresh_status = fresh_reads_->freshRead(
          fresh_keys, snapshot.catalog_generation, request.deadline);
    }

    now_ns = clock_->monotonicNow().time_since_epoch().count();
    snapshot = data_manager_.readLatest(request.selection, request.allow_stale,
                                        now_ns, request.catalog_generation);
  }

  if (request.consistency == QueryConsistency::kAtLeastEpoch &&
      isReadableStatus(snapshot.status)) {
    while (!allItemsAtEpoch(snapshot, request.required_epoch)) {
      const std::uint64_t next_epoch = data_manager_.commitEpoch() + 1;
      const Status wait_status =
          data_manager_.waitForEpoch(next_epoch, request.deadline);
      if (!wait_status.ok()) {
        snapshot.status = wait_status;
        snapshot.items.clear();
        break;
      }
      now_ns = clock_->monotonicNow().time_since_epoch().count();
      snapshot =
          data_manager_.readLatest(request.selection, request.allow_stale,
                                   now_ns, request.catalog_generation);
      if (!isReadableStatus(snapshot.status)) {
        break;
      }
    }
  }

  return finalize(request, std::move(snapshot), fresh_status);
}

} // namespace pdcm
