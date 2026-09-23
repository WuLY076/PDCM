#include "common/observation.hpp"

namespace pdcm {

Status validateObservation(const Observation &observation) {
  if (observation.entity.kind == EntityKind::kUnknown ||
      observation.entity.generation == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "observation must reference a current entity generation");
  }

  const bool requires_value = observation.status == ObservationStatus::kValid ||
                              observation.status == ObservationStatus::kStale;
  if (requires_value && !observation.value.has_value()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "valid and stale observations require a value");
  }
  if (!requires_value && observation.value.has_value()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "failed observations must not contain a value");
  }

  if (observation.observed_monotonic_time_ns < 0 ||
      observation.catalog_generation == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "observation time and catalog generation are invalid");
  }

  return Status::success();
}

} // namespace pdcm
