#ifndef PDCM_COMMON_OBSERVATION_HPP_
#define PDCM_COMMON_OBSERVATION_HPP_

#include <cstdint>
#include <optional>
#include <string>

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

struct Observation {
  EntityRef entity;
  MetricId metric;
  std::optional<MetricValue> value;
  ObservationStatus status{ObservationStatus::kNotAvailable};
  std::optional<std::int64_t> source_sample_time_ns;
  std::int64_t observed_monotonic_time_ns{0};
  std::int64_t observed_wall_time_ns{0};
  std::uint64_t catalog_generation{0};
  std::uint64_t commit_epoch{0};
  SourceMetadata source;
  ErrorMetadata error;
};

[[nodiscard]] Status validateObservation(const Observation &observation);

} // namespace pdcm

#endif // PDCM_COMMON_OBSERVATION_HPP_
