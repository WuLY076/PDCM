#include <memory>

#include <gtest/gtest.h>

#include "core/service_core.hpp"
#include "pdcm/testkit/manual_clock.hpp"
#include "pdcm/testkit/mock_provider.hpp"

namespace pdcm::testkit {
namespace {

RuntimeConfig config() {
  RuntimeConfig value;
  value.target = TargetKind::kFpga;
  return value;
}

std::unique_ptr<MockProvider> provider(MockScenario scenario,
                                       const std::shared_ptr<Clock> &clock) {
  MockProviderConfig provider_config;
  provider_config.scenario = scenario;
  return std::make_unique<MockProvider>(provider_config, clock);
}

TEST(CoreLifecycleTest, SingleAndZeroDeviceReachReady) {
  auto clock = std::make_shared<ManualClock>(100, 1000);

  PdcmServiceCore single(config(), provider(MockScenario::kSingleDevice, clock),
                         clock);
  ASSERT_TRUE(single.start().ok());
  CoreSnapshot single_snapshot = single.snapshot();
  EXPECT_EQ(single_snapshot.state, CoreState::kReady);
  EXPECT_EQ(single_snapshot.provider_state, ProviderState::kReady);
  EXPECT_EQ(single_snapshot.detected_device_count, 1);
  EXPECT_EQ(single_snapshot.catalog_generation, 1);
  const std::shared_ptr<const CatalogView> single_catalog =
      single.catalogSnapshot();
  ASSERT_EQ(single_catalog->entities().size(), 1);
  EXPECT_EQ(single_catalog->generation(), single_snapshot.catalog_generation);
  EXPECT_EQ(single_catalog->entities().front().ref.id, EntityId{0});
  EXPECT_EQ(single_catalog->entities().front().native_id.value,
            "mock-device-0");
  EXPECT_EQ(single_catalog->entities().front().pci_bdf.value, "0000:01:00.0");
  EXPECT_EQ(single_catalog->entities().front().pdrv_version.value,
            "mock-pdrv-1.0");

  PdcmServiceCore zero(config(), provider(MockScenario::kZeroDevice, clock),
                       clock);
  ASSERT_TRUE(zero.start().ok());
  CoreSnapshot zero_snapshot = zero.snapshot();
  EXPECT_EQ(zero_snapshot.state, CoreState::kReady);
  EXPECT_EQ(zero_snapshot.detected_device_count, 0);
  EXPECT_EQ(zero.catalogSnapshot()->generation(), 1);
  EXPECT_TRUE(zero.catalogSnapshot()->entities().empty());
}

TEST(CoreLifecycleTest, MultipleDevicesEnterDiagnosableDegradedState) {
  auto clock = std::make_shared<ManualClock>(100, 1000);
  PdcmServiceCore core(config(),
                       provider(MockScenario::kMultipleDevices, clock), clock);

  ASSERT_TRUE(core.start().ok());
  CoreSnapshot snapshot = core.snapshot();
  EXPECT_EQ(snapshot.state, CoreState::kDegraded);
  EXPECT_EQ(snapshot.degraded_reason, CoreDegradedReason::kTopologyUnsupported);
  EXPECT_EQ(snapshot.detail_status, PDCM_STATUS_UNSUPPORTED);
  EXPECT_EQ(snapshot.detected_device_count, 2);
  EXPECT_EQ(snapshot.catalog_generation, 1);
  EXPECT_TRUE(core.catalogSnapshot()->topologyUnsupported());
  EXPECT_EQ(core.catalogSnapshot()->detectedDeviceCount(), 2);
  EXPECT_TRUE(core.catalogSnapshot()->entities().empty());
}

TEST(CoreLifecycleTest, ProviderFailureDoesNotFailCoreStartup) {
  auto clock = std::make_shared<ManualClock>(100, 1000);
  PdcmServiceCore core(
      config(), provider(MockScenario::kProviderUnavailable, clock), clock);

  ASSERT_TRUE(core.start().ok());
  CoreSnapshot snapshot = core.snapshot();
  EXPECT_EQ(snapshot.state, CoreState::kDegraded);
  EXPECT_EQ(snapshot.provider_state, ProviderState::kUnavailable);
  EXPECT_EQ(snapshot.degraded_reason, CoreDegradedReason::kProviderUnavailable);
  EXPECT_EQ(snapshot.detail_status, PDCM_STATUS_UNAVAILABLE);
}

TEST(CoreLifecycleTest, StopIsIdempotentAndRejectsRestart) {
  auto clock = std::make_shared<ManualClock>(100, 1000);
  auto mock = provider(MockScenario::kSingleDevice, clock);
  MockProvider *mock_observer = mock.get();
  PdcmServiceCore core(config(), std::move(mock), clock);

  ASSERT_TRUE(core.start().ok());
  EXPECT_TRUE(core.stop().ok());
  EXPECT_TRUE(core.stop().ok());
  EXPECT_EQ(core.snapshot().state, CoreState::kStopped);
  EXPECT_EQ(mock_observer->state(), ProviderState::kShutdown);
  EXPECT_EQ(core.start().code(), PDCM_STATUS_NOT_INITIALIZED);
}

TEST(CoreLifecycleTest, InvalidConfigurationFailsCoreInvariant) {
  auto clock = std::make_shared<ManualClock>(100, 1000);
  RuntimeConfig invalid;
  PdcmServiceCore core(invalid, provider(MockScenario::kSingleDevice, clock),
                       clock);

  EXPECT_EQ(core.start().code(), PDCM_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(core.snapshot().state, CoreState::kFailed);
  EXPECT_TRUE(core.stop().ok());
  EXPECT_EQ(core.snapshot().state, CoreState::kStopped);
}

} // namespace
} // namespace pdcm::testkit
