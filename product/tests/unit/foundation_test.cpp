#include <cstdint>
#include <string_view>
#include <type_traits>

#include <gtest/gtest.h>

#include "common/health.hpp"
#include "common/observation.hpp"
#include "common/status.hpp"
#include "provider/provider.hpp"

#include "semantic/catalog_types.hpp"
namespace pdcm {
namespace {

TEST(StatusTest, ExposesStableStatusNames) {
  EXPECT_EQ(std::string_view(statusName(PDCM_STATUS_PROTOCOL_INCOMPATIBLE)),
            "PROTOCOL_INCOMPATIBLE");
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

TEST(HealthContractTest, EnforcesEvidenceAndStateInvariants) {
  FirmwareHeartbeatEvidence evidence;
  evidence.entity = EntityRef{EntityKind::kDevice, EntityId{0}, 1};
  evidence.provider_data_id = 7;
  evidence.native_class = HeartbeatNativeClass::kNormal;
  evidence.status = ObservationStatus::kValid;
  evidence.sequence_or_token = 9;
  evidence.source_sample_time_ns = 100;
  evidence.observed_monotonic_time_ns = 110;
  evidence.source.provider = "normalized-provider";
  evidence.catalog_generation = 1;
  EXPECT_TRUE(validateFirmwareHeartbeatEvidence(evidence).ok());

  FirmwareHeartbeatEvidence unknown_native = evidence;
  unknown_native.native_class = HeartbeatNativeClass::kUnknownNative;
  EXPECT_EQ(validateFirmwareHeartbeatEvidence(unknown_native).code(),
            PDCM_STATUS_INVALID_ARGUMENT);

  FirmwareHeartbeatEvidence timeout = evidence;
  timeout.native_class = HeartbeatNativeClass::kUnknownNative;
  timeout.status = ObservationStatus::kError;
  timeout.source_sample_time_ns.reset();
  timeout.error.status = Status(PDCM_STATUS_TIMEOUT, "heartbeat timed out");
  EXPECT_TRUE(validateFirmwareHeartbeatEvidence(timeout).ok());

  HealthResult healthy;
  healthy.entity = evidence.entity;
  healthy.subsystem_id = kFirmwareHeartbeatHealthId;
  healthy.state = HealthState::kHealthy;
  healthy.item_status = ObservationStatus::kValid;
  healthy.catalog_generation = 1;
  healthy.evaluated_monotonic_time_ns = 120;
  healthy.evidence_age_ns = 10;
  healthy.evidence.push_back(
      EvidenceRef{1, evidence.sequence_or_token, evidence.source_sample_time_ns,
                  evidence.observed_monotonic_time_ns, evidence.source});
  healthy.code = StableHealthCode::kHeartbeatOk;
  EXPECT_TRUE(validateHealthResult(healthy).ok());

  HealthResult false_healthy = healthy;
  false_healthy.item_status = ObservationStatus::kError;
  EXPECT_EQ(validateHealthResult(false_healthy).code(),
            PDCM_STATUS_INVALID_ARGUMENT);

  HealthResult unknown = healthy;
  unknown.state = HealthState::kUnknown;
  unknown.item_status = ObservationStatus::kError;
  unknown.code = StableHealthCode::kHeartbeatTimeout;
  unknown.limitations.push_back(
      {HealthLimitationCode::kReadFailure, "heartbeat read timed out"});
  EXPECT_TRUE(validateHealthResult(unknown).ok());

  HealthResult timeout_is_not_fault = unknown;
  timeout_is_not_fault.state = HealthState::kError;
  EXPECT_EQ(validateHealthResult(timeout_is_not_fault).code(),
            PDCM_STATUS_INVALID_ARGUMENT);
}

} // namespace
} // namespace pdcm
