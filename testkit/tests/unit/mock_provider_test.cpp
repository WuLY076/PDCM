#include <chrono>
#include <cstdint>
#include <memory>
#include <variant>

#include <gtest/gtest.h>

#include "pdcm/testkit/manual_clock.hpp"
#include "pdcm/testkit/mock_metric_ids.hpp"
#include "pdcm/testkit/mock_provider.hpp"

namespace pdcm::testkit {
namespace {

ProviderInitOptions initOptions(const ManualClock &clock) {
  ProviderInitOptions options;
  options.target = TargetKind::kFpga;
  options.deadline = clock.monotonicNow() + std::chrono::seconds(1);
  return options;
}

ProviderReadItem readItem(const ProviderDataKind kind, const std::uint32_t id) {
  ProviderReadItem item;
  item.entity = EntityRef{EntityKind::kDevice, EntityId{0}, 1};
  item.kind = kind;
  item.data_id = id;
  return item;
}

ProviderReadRequest readRequest(const ManualClock &clock) {
  ProviderReadRequest request;
  request.request_id = 1;
  request.plan_generation = 1;
  request.catalog_generation = 1;
  request.scheduled_time = clock.monotonicNow();
  request.deadline = clock.monotonicNow() + std::chrono::seconds(1);
  return request;
}

TEST(MockProviderTest, ProducesDeterministicSingleDeviceData) {
  auto clock = std::make_shared<ManualClock>(100, 1000);
  MockProvider provider(MockProviderConfig{}, clock);

  ASSERT_TRUE(provider.initialize(initOptions(*clock)).ok());
  EXPECT_EQ(provider.state(), ProviderState::kReady);
  EXPECT_EQ(provider.concurrency().max_concurrency, 1);

  ProviderDiscoveryResult discovery =
      provider.discover(clock->monotonicNow() + std::chrono::seconds(1));
  ASSERT_TRUE(discovery.call_status.ok());
  EXPECT_EQ(discovery.descriptor.detected_device_count, 1);
  EXPECT_EQ(discovery.descriptor.entities.size(), 1);
  EXPECT_EQ(discovery.descriptor.capabilities.size(), 4);

  ProviderReadRequest request = readRequest(*clock);
  request.items = {
      readItem(ProviderDataKind::kMetric, kTestGaugeMetricId),
      readItem(ProviderDataKind::kMetric, kTestCounterMetricId),
      readItem(ProviderDataKind::kHeartbeatEvidence, kTestHeartbeatEvidenceId),
  };

  ProviderReadResult first = provider.batchRead(request);
  ASSERT_TRUE(first.call_status.ok());
  ASSERT_EQ(first.items.size(), request.items.size());
  EXPECT_EQ(std::get<std::uint64_t>(*first.items[0].value), 10);
  EXPECT_EQ(std::get<std::uint64_t>(*first.items[1].value), 100);
  EXPECT_FALSE(first.items[2].value.has_value());
  ASSERT_TRUE(first.items[2].heartbeat_class.has_value());
  EXPECT_EQ(*first.items[2].heartbeat_class, HeartbeatNativeClass::kNormal);
  ASSERT_TRUE(first.items[2].sequence_or_token.has_value());
  EXPECT_EQ(*first.items[2].sequence_or_token, 1);

  ProviderReadResult second = provider.batchRead(request);
  EXPECT_EQ(std::get<std::uint64_t>(*second.items[0].value), 20);
}

TEST(MockProviderTest, PreservesPartialItemFailures) {
  auto clock = std::make_shared<ManualClock>(100, 1000);
  MockProviderConfig config;
  config.scenario = MockScenario::kPartialMetricFailure;
  MockProvider provider(config, clock);
  ASSERT_TRUE(provider.initialize(initOptions(*clock)).ok());

  ProviderReadRequest request = readRequest(*clock);
  request.items = {
      readItem(ProviderDataKind::kMetric, kTestGaugeMetricId),
      readItem(ProviderDataKind::kMetric, kTestPeriodicFailureMetricId),
  };

  ProviderReadResult result = provider.batchRead(request);
  ASSERT_EQ(result.call_status.code(), PDCM_STATUS_PARTIAL_RESULT);
  ASSERT_EQ(result.items.size(), 2);
  EXPECT_EQ(result.items[0].status, ObservationStatus::kValid);
  EXPECT_EQ(result.items[1].status, ObservationStatus::kError);
  EXPECT_FALSE(result.items[1].value.has_value());
}

TEST(MockProviderTest, ExposesZeroAndMultipleDeviceFixtures) {
  auto clock = std::make_shared<ManualClock>(100, 1000);

  MockProviderConfig zero_config;
  zero_config.scenario = MockScenario::kZeroDevice;
  MockProvider zero(zero_config, clock);
  ASSERT_TRUE(zero.initialize(initOptions(*clock)).ok());
  EXPECT_EQ(zero.discover(clock->monotonicNow() + std::chrono::seconds(1))
                .descriptor.detected_device_count,
            0);

  MockProviderConfig multiple_config;
  multiple_config.scenario = MockScenario::kMultipleDevices;
  MockProvider multiple(multiple_config, clock);
  ASSERT_TRUE(multiple.initialize(initOptions(*clock)).ok());
  ProviderDiscoveryResult result =
      multiple.discover(clock->monotonicNow() + std::chrono::seconds(1));
  EXPECT_EQ(result.descriptor.detected_device_count, 2);
  EXPECT_EQ(result.descriptor.entities.size(), 2);
}

TEST(MockProviderTest, ReportsUnavailableTimeoutAndShutdown) {
  auto clock = std::make_shared<ManualClock>(100, 1000);

  MockProviderConfig unavailable_config;
  unavailable_config.scenario = MockScenario::kProviderUnavailable;
  MockProvider unavailable(unavailable_config, clock);
  EXPECT_EQ(unavailable.initialize(initOptions(*clock)).code(),
            PDCM_STATUS_UNAVAILABLE);
  EXPECT_EQ(unavailable.state(), ProviderState::kUnavailable);

  MockProvider provider(MockProviderConfig{}, clock);
  ASSERT_TRUE(provider.initialize(initOptions(*clock)).ok());
  ProviderReadRequest expired = readRequest(*clock);
  expired.items = {
      readItem(ProviderDataKind::kMetric, kTestGaugeMetricId),
  };
  clock->advance(std::chrono::seconds(2));

  ProviderReadResult timeout = provider.batchRead(expired);
  EXPECT_EQ(timeout.call_status.code(), PDCM_STATUS_TIMEOUT);
  ASSERT_EQ(timeout.items.size(), 1);
  EXPECT_EQ(timeout.items[0].status, ObservationStatus::kError);

  provider.shutdown();
  EXPECT_EQ(provider.state(), ProviderState::kShutdown);
  provider.shutdown();
  EXPECT_EQ(provider.batchRead(readRequest(*clock)).call_status.code(),
            PDCM_STATUS_UNAVAILABLE);
}

TEST(ManualClockTest, RejectsBackwardMovement) {
  ManualClock clock(100, 1000);
  EXPECT_THROW(clock.advance(std::chrono::nanoseconds(-1)),
               std::invalid_argument);
}

} // namespace
} // namespace pdcm::testkit
