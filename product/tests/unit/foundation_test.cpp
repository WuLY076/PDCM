#include <cstdint>
#include <iostream>
#include <string_view>
#include <type_traits>

#include "common/observation.hpp"
#include "common/status.hpp"
#include "provider/provider.hpp"

namespace {

int failures = 0;

void check(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}

} // namespace

int main() {
  using namespace pdcm;

  check(std::string_view(statusName(PDCM_STATUS_STALE_GENERATION)) ==
            "STALE_GENERATION",
        "stable status name");
  check(std::has_virtual_destructor_v<Provider>,
        "provider must be safely destructible through its interface");

  Observation valid;
  valid.entity = EntityRef{EntityKind::kDevice, EntityId{0}, 1};
  valid.metric = MetricId{1};
  valid.value = std::uint64_t{42};
  valid.status = ObservationStatus::kValid;
  valid.observed_monotonic_time_ns = 10;
  valid.catalog_generation = 1;
  check(validateObservation(valid).ok(), "valid observation accepted");

  Observation failed = valid;
  failed.status = ObservationStatus::kError;
  check(validateObservation(failed).code() == PDCM_STATUS_INVALID_ARGUMENT,
        "error observation cannot carry a value");

  failed.value.reset();
  check(validateObservation(failed).ok(),
        "error observation without a value accepted");

  Observation stale = valid;
  stale.status = ObservationStatus::kStale;
  check(validateObservation(stale).ok(), "stale observation keeps old value");

  ProviderDescriptor descriptor;
  descriptor.state = ProviderState::kUnavailable;
  descriptor.detected_device_count = 0;
  check(descriptor.entities.empty(), "provider descriptor starts empty");

  return failures == 0 ? 0 : 1;
}
