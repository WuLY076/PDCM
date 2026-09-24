#include "common/observation.hpp"

namespace pdcm {

Status validateObservation(const Observation &observation) {
  if (observation.entity.kind == EntityKind::kUnknown ||
      observation.entity.generation == 0 || observation.metric.value == 0 ||
      observation.metric_semantic_version == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "observation identity or semantic version is invalid");
  }
  if (observation.observed_monotonic_time_ns < 0 ||
      observation.catalog_generation == 0 ||
      (observation.source_sample_time_ns.has_value() &&
       *observation.source_sample_time_ns < 0)) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "observation time or catalog generation is invalid");
  }

  const bool has_value = observation.value.has_value();
  switch (observation.status) {
  case ObservationStatus::kValid:
    if (!has_value || !observation.error.status.ok() ||
        observation.stale_age_ns.has_value() ||
        observation.latest_failure.has_value()) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "valid observation metadata is inconsistent");
    }
    break;
  case ObservationStatus::kStale:
    if (!has_value || !observation.stale_age_ns.has_value() ||
        *observation.stale_age_ns < 0 ||
        !observation.latest_failure.has_value() ||
        observation.latest_failure->status.ok()) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "stale observation requires old value and latest failure");
    }
    break;
  case ObservationStatus::kNotAvailable:
  case ObservationStatus::kUnsupported:
  case ObservationStatus::kError:
    if (has_value || observation.error.status.ok() ||
        observation.stale_age_ns.has_value() ||
        observation.latest_failure.has_value()) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "failed observation metadata is inconsistent");
    }
    break;
  }

  if (observation.derivation.depth > 4) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "observation derivation depth exceeds limit");
  }
  if (observation.derivation.processor_id.empty()) {
    if (observation.derivation.depth != 0 ||
        observation.derivation.processor_version != 0 ||
        !observation.derivation.input_sequences.empty()) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "raw observation has derived provenance");
    }
  } else if (observation.derivation.depth == 0 ||
             observation.derivation.processor_version == 0 ||
             observation.derivation.input_sequences.empty()) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "derived observation provenance is incomplete");
  }

  return Status::success();
}

} // namespace pdcm
