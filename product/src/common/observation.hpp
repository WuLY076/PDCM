#ifndef PDCM_COMMON_OBSERVATION_HPP_
#define PDCM_COMMON_OBSERVATION_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/domain_types.hpp"
#include "common/status.hpp"

namespace pdcm {

enum class ObservationStatus : std::uint8_t {
  kValid,
  kStale,
  kNotAvailable,
  kUnsupported,
  kError,
};

struct SourceMetadata {
  std::string provider;
  std::string native_source;
};

struct ErrorMetadata {
  Status status;
  std::int64_t native_code{0};
  bool retryable{false};
};

struct DerivationMetadata {
  std::string processor_id;
  std::vector<std::uint64_t> input_sequences;
  std::uint32_t processor_version{0};
  std::uint8_t depth{0};
};

struct Observation {
  EntityRef entity;
  MetricId metric;
  std::optional<MetricValue> value;
  ObservationStatus status{ObservationStatus::kNotAvailable};
  std::int64_t scheduled_monotonic_time_ns{0};
  std::optional<std::int64_t> source_sample_time_ns;
  std::int64_t observed_monotonic_time_ns{0};
  std::uint32_t metric_semantic_version{0};
  std::int64_t observed_wall_time_ns{0};
  std::uint64_t catalog_generation{0};
  std::uint64_t commit_epoch{0};
  std::uint64_t counter_epoch{0};
  SourceMetadata source;
  std::optional<std::int64_t> stale_age_ns;
  std::optional<ErrorMetadata> latest_failure;
  ErrorMetadata error;
  DerivationMetadata derivation;
};

[[nodiscard]] Status validateObservation(const Observation &observation);

} // namespace pdcm

#endif // PDCM_COMMON_OBSERVATION_HPP_
