#include <cstdint>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include "common/observation.hpp"
#include "common/status.hpp"
#include "provider/provider.hpp"

namespace pdcm {
namespace {

TEST(StatusTest, ExposesStableStatusNames) {
  EXPECT_EQ(std::string_view(statusName(PDCM_STATUS_STALE_GENERATION)),
            "STALE_GENERATION");
}

TEST(ProviderContractTest, HasSafePolymorphicDestruction) {
  EXPECT_TRUE(std::has_virtual_destructor_v<Provider>);

  ProviderDescriptor descriptor;
  descriptor.state = ProviderState::kUnavailable;
  descriptor.detected_device_count = 0;
  EXPECT_TRUE(descriptor.entities.empty());
}

TEST(ObservationTest, EnforcesStatusAndValueInvariants) {
  Observation valid;
  valid.entity = EntityRef{EntityKind::kDevice, EntityId{0}, 1};
  valid.metric = MetricId{1};
  valid.metric_semantic_version = 1;
  valid.value = std::uint64_t{42};
  valid.status = ObservationStatus::kValid;
  valid.observed_monotonic_time_ns = 10;
  valid.catalog_generation = 1;
  EXPECT_TRUE(validateObservation(valid).ok());

  Observation failed = valid;
  failed.status = ObservationStatus::kError;
  EXPECT_EQ(validateObservation(failed).code(), PDCM_STATUS_INVALID_ARGUMENT);

  failed.value.reset();
  failed.error.status = Status(PDCM_STATUS_UNAVAILABLE, "provider read failed");
  EXPECT_TRUE(validateObservation(failed).ok());

  Observation stale = valid;
  stale.status = ObservationStatus::kStale;
  stale.stale_age_ns = 5;
  stale.latest_failure = ErrorMetadata{
      Status(PDCM_STATUS_UNAVAILABLE, "provider read failed"), 0, true};
  EXPECT_TRUE(validateObservation(stale).ok());
}

} // namespace
} // namespace pdcm
