#include "metrics/metrics_manager.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

#include <gtest/gtest.h>

namespace pdcm {
namespace {

constexpr std::uint32_t kTestHeartbeatDataId = 0xF0000004U;

class TestClock final : public Clock {
public:
  explicit TestClock(const std::int64_t now = 0) : now_(Nanoseconds(now)) {}

  [[nodiscard]] MonotonicTime monotonicNow() const noexcept override {
    return now_;
  }

  [[nodiscard]] std::int64_t wallTimeNanoseconds() const noexcept override {
    return now_.time_since_epoch().count() + 1000;
  }

  void set(const std::int64_t now) { now_ = MonotonicTime(Nanoseconds(now)); }

private:
  MonotonicTime now_;
};

TargetCatalog healthCatalog() {
  TargetCatalog catalog = TargetCatalog::blocked(TargetKind::kFpga);
  catalog.health.front().provider_data_id = kTestHeartbeatDataId;
  catalog.health.front().freshness_ns = 100;
  return catalog;
}

ProviderDescriptor providerDescriptor(const bool heartbeat_supported = true) {
  ProviderDescriptor descriptor;
  descriptor.provider_version = "metrics-test-provider";
  descriptor.target = TargetKind::kFpga;
  descriptor.state = ProviderState::kReady;
  descriptor.detected_device_count = 1;
  ProviderEntity entity;
  entity.stable_native_id = "metrics-device";
  entity.pci_bdf = "0000:01:00.0";
  entity.pdrv_version = "metrics-pdrv";
  entity.incarnation = "boot-1";
  entity.state = ProviderEntityState::kReady;
  entity.manageable = true;
  descriptor.entities.push_back(std::move(entity));
  descriptor.capabilities.push_back(ProviderCapability{
      ProviderDataKind::kHeartbeatEvidence, kTestHeartbeatDataId,
      heartbeat_supported,
      heartbeat_supported ? "TEST_SUPPORTED" : "TEST_UNSUPPORTED"});
  return descriptor;
}

struct FixtureState {
  TargetCatalog target{healthCatalog()};
  SemanticCatalog semantic{target};
  std::shared_ptr<const CatalogView> view;
  EntityRef entity;
  DataManager data;
  TestClock clock;
  MetricsManager metrics{data, clock};

  explicit FixtureState(const bool heartbeat_supported = true) {
    const CatalogCommitResult catalog_commit =
        semantic.commit(providerDescriptor(heartbeat_supported));
    if (!catalog_commit.status.ok() || !catalog_commit.committed) {
      throw std::runtime_error("test catalog commit failed");
    }
    view = semantic.snapshot();
    entity = view->entities().front().ref;
    if (!data.activateCatalog(view, target).ok() ||
        !metrics.activateCatalog(view, target).ok()) {
      throw std::runtime_error("test managers activation failed");
    }
  }
};

FirmwareHeartbeatEvidence
validEvidence(const FixtureState &fixture, const std::uint64_t token,
              const std::int64_t observed_time,
              const HeartbeatNativeClass native_class) {
  FirmwareHeartbeatEvidence evidence;
  evidence.entity = fixture.entity;
  evidence.provider_data_id = kTestHeartbeatDataId;
  evidence.native_class = native_class;
  evidence.status = ObservationStatus::kValid;
  evidence.sequence_or_token = token;
  evidence.source_sample_time_ns = observed_time;
  evidence.observed_monotonic_time_ns = observed_time;
  evidence.source.provider = "mock-provider";
  evidence.source.native_source = "mock-heartbeat";
  evidence.catalog_generation = fixture.view->generation();
  return evidence;
}

FirmwareHeartbeatEvidence timeoutEvidence(const FixtureState &fixture,
                                          const std::int64_t observed_time) {
  FirmwareHeartbeatEvidence evidence;
  evidence.entity = fixture.entity;
  evidence.provider_data_id = kTestHeartbeatDataId;
  evidence.status = ObservationStatus::kError;
  evidence.observed_monotonic_time_ns = observed_time;
  evidence.source.provider = "mock-provider";
  evidence.source.native_source = "mock-heartbeat";
  evidence.error.status =
      Status(PDCM_STATUS_TIMEOUT, "scripted heartbeat timeout");
  evidence.error.retryable = true;
  evidence.catalog_generation = fixture.view->generation();
  return evidence;
}

EvidenceCommitResult commitEvidence(FixtureState &fixture,
                                    FirmwareHeartbeatEvidence evidence) {
  const EvidenceCommitResult committed =
      fixture.data.commitHeartbeatEvidence(std::move(evidence));
  if (committed.status.ok() && committed.committed) {
    const Status evaluated =
        fixture.metrics.onHeartbeatEvidenceCommitted(committed.evidence_id);
    if (!evaluated.ok()) {
      throw std::runtime_error("test evidence evaluation failed");
    }
  }
  return committed;
}

TEST(MetricsManagerTest, StartsUnknownAndReportsUnsupportedMappingExplicitly) {
  FixtureState supported;
  const HealthReadResult initial =
      supported.data.readHealth(supported.entity, supported.view->generation());
  ASSERT_TRUE(initial.status.ok());
  ASSERT_TRUE(initial.result.has_value());
  EXPECT_EQ(initial.result->state, HealthState::kUnknown);
  EXPECT_EQ(initial.result->code, StableHealthCode::kHeartbeatMissing);
  EXPECT_EQ(initial.result->item_status, ObservationStatus::kNotAvailable);

  FixtureState unsupported(false);
  const HealthQueryResult queried = unsupported.metrics.queryHealth(
      HealthRequest{unsupported.entity, kFirmwareHeartbeatHealthId,
                    unsupported.view->generation(), 0});
  EXPECT_EQ(queried.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  ASSERT_TRUE(queried.item.has_value());
  EXPECT_EQ(queried.item->state, HealthState::kUnknown);
  EXPECT_EQ(queried.item->code, StableHealthCode::kHeartbeatUnsupported);
  EXPECT_EQ(queried.item->item_status, ObservationStatus::kUnsupported);
}

TEST(MetricsManagerTest, MapsOnlyExplicitNativeClassesToDeterminateHealth) {
  FixtureState fixture;

  fixture.clock.set(10);
  ASSERT_TRUE(
      commitEvidence(
          fixture, validEvidence(fixture, 1, 10, HeartbeatNativeClass::kNormal))
          .status.ok());
  HealthQueryResult queried = fixture.metrics.queryHealth(
      HealthRequest{fixture.entity, kFirmwareHeartbeatHealthId,
                    fixture.view->generation(), 0});
  ASSERT_TRUE(queried.status.ok());
  ASSERT_TRUE(queried.item.has_value());
  EXPECT_EQ(queried.item->state, HealthState::kHealthy);
  EXPECT_EQ(queried.item->code, StableHealthCode::kHeartbeatOk);

  fixture.clock.set(20);
  ASSERT_TRUE(
      commitEvidence(fixture, validEvidence(fixture, 2, 20,
                                            HeartbeatNativeClass::kWarning))
          .status.ok());
  queried = fixture.metrics.queryHealth(
      HealthRequest{fixture.entity, kFirmwareHeartbeatHealthId,
                    fixture.view->generation(), 0});
  ASSERT_TRUE(queried.item.has_value());
  EXPECT_EQ(queried.item->state, HealthState::kWarning);
  EXPECT_EQ(queried.item->code, StableHealthCode::kHeartbeatWarning);

  fixture.clock.set(30);
  ASSERT_TRUE(
      commitEvidence(
          fixture, validEvidence(fixture, 3, 30, HeartbeatNativeClass::kFault))
          .status.ok());
  queried = fixture.metrics.queryHealth(
      HealthRequest{fixture.entity, kFirmwareHeartbeatHealthId,
                    fixture.view->generation(), 0});
  ASSERT_TRUE(queried.item.has_value());
  EXPECT_EQ(queried.item->state, HealthState::kError);
  EXPECT_EQ(queried.item->code, StableHealthCode::kHeartbeatFault);
}

TEST(MetricsManagerTest, ExpiresEvidenceAndRequiresStrictlyNewRecovery) {
  FixtureState fixture;
  fixture.clock.set(10);
  ASSERT_TRUE(
      commitEvidence(
          fixture, validEvidence(fixture, 1, 10, HeartbeatNativeClass::kNormal))
          .status.ok());

  ASSERT_TRUE(
      fixture.metrics.runFreshnessOnce(MonotonicTime(Nanoseconds(111))).ok());
  HealthReadResult stored =
      fixture.data.readHealth(fixture.entity, fixture.view->generation());
  ASSERT_TRUE(stored.result.has_value());
  EXPECT_EQ(stored.result->state, HealthState::kUnknown);
  EXPECT_EQ(stored.result->code, StableHealthCode::kHeartbeatStale);

  EXPECT_EQ(fixture.data
                .commitHeartbeatEvidence(validEvidence(
                    fixture, 1, 120, HeartbeatNativeClass::kNormal))
                .status.code(),
            PDCM_STATUS_STALE_GENERATION);

  fixture.clock.set(120);
  ASSERT_TRUE(
      commitEvidence(fixture, validEvidence(fixture, 2, 120,
                                            HeartbeatNativeClass::kNormal))
          .status.ok());
  stored = fixture.data.readHealth(fixture.entity, fixture.view->generation());
  ASSERT_TRUE(stored.result.has_value());
  EXPECT_EQ(stored.result->state, HealthState::kHealthy);

  fixture.clock.set(130);
  ASSERT_TRUE(
      commitEvidence(fixture, timeoutEvidence(fixture, 130)).status.ok());
  stored = fixture.data.readHealth(fixture.entity, fixture.view->generation());
  ASSERT_TRUE(stored.result.has_value());
  EXPECT_EQ(stored.result->state, HealthState::kUnknown);
  EXPECT_EQ(stored.result->code, StableHealthCode::kHeartbeatTimeout);

  fixture.clock.set(140);
  FirmwareHeartbeatEvidence read_error = timeoutEvidence(fixture, 140);
  read_error.error.status =
      Status(PDCM_STATUS_INTERNAL, "scripted heartbeat read error");
  ASSERT_TRUE(commitEvidence(fixture, std::move(read_error)).status.ok());
  stored = fixture.data.readHealth(fixture.entity, fixture.view->generation());
  ASSERT_TRUE(stored.result.has_value());
  EXPECT_EQ(stored.result->state, HealthState::kUnknown);
  EXPECT_EQ(stored.result->code, StableHealthCode::kHeartbeatReadError);
}

TEST(MetricsManagerTest, KeepsUnsupportedSubsystemOutOfAggregateHealth) {
  FixtureState fixture;
  fixture.clock.set(10);
  ASSERT_TRUE(
      commitEvidence(
          fixture, validEvidence(fixture, 1, 10, HeartbeatNativeClass::kNormal))
          .status.ok());

  const HealthQueryResult queried = fixture.metrics.queryHealth(
      HealthRequest{fixture.entity, 99, fixture.view->generation(), 0});
  EXPECT_EQ(queried.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  ASSERT_TRUE(queried.item.has_value());
  EXPECT_EQ(queried.item->subsystem_id, 99);
  EXPECT_EQ(queried.item->state, HealthState::kUnknown);
  EXPECT_EQ(queried.item->item_status, ObservationStatus::kUnsupported);
  ASSERT_TRUE(queried.aggregate.has_value());
  EXPECT_EQ(queried.aggregate->subsystem_id, kFirmwareHeartbeatHealthId);
  EXPECT_EQ(queried.aggregate->state, HealthState::kHealthy);

  fixture.clock.set(20);
  ASSERT_TRUE(
      fixture.metrics.onProviderStateChanged(ProviderState::kUnavailable).ok());
  const HealthReadResult unavailable =
      fixture.data.readHealth(fixture.entity, fixture.view->generation());
  ASSERT_TRUE(unavailable.result.has_value());
  EXPECT_EQ(unavailable.result->state, HealthState::kUnknown);
  EXPECT_EQ(unavailable.result->code, StableHealthCode::kProviderUnavailable);
}

TEST(MetricsManagerTest, AppliesCallerMaxAgeWithoutMutatingStoredHealth) {
  FixtureState fixture;
  fixture.clock.set(10);
  ASSERT_TRUE(
      commitEvidence(
          fixture, validEvidence(fixture, 1, 10, HeartbeatNativeClass::kNormal))
          .status.ok());

  fixture.clock.set(20);
  const HealthQueryResult queried = fixture.metrics.queryHealth(
      HealthRequest{fixture.entity, kFirmwareHeartbeatHealthId,
                    fixture.view->generation(), 5});
  EXPECT_EQ(queried.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  ASSERT_TRUE(queried.item.has_value());
  EXPECT_EQ(queried.item->state, HealthState::kUnknown);
  EXPECT_EQ(queried.item->code, StableHealthCode::kHeartbeatStale);

  const HealthReadResult stored =
      fixture.data.readHealth(fixture.entity, fixture.view->generation());
  ASSERT_TRUE(stored.result.has_value());
  EXPECT_EQ(stored.result->state, HealthState::kHealthy);
}

} // namespace
} // namespace pdcm
