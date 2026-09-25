#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "collection/collection_coordinator.hpp"
#include "collection/watch_manager.hpp"
#include "metrics/metrics_manager.hpp"
#include "pdcm/testkit/manual_clock.hpp"
#include "pdcm/testkit/mock_metric_ids.hpp"
#include "pdcm/testkit/mock_provider.hpp"
#include "provider/provider_manager.hpp"
#include "semantic/semantic_catalog.hpp"

namespace pdcm::testkit {
namespace {

TargetCatalog healthCatalog() {
  TargetCatalog catalog = TargetCatalog::blocked(TargetKind::kFpga);
  catalog.health.front().provider_data_id = kTestHeartbeatEvidenceId;
  catalog.health.front().freshness_ns = 100;
  return catalog;
}

struct HealthRoundTrip {
  std::shared_ptr<ManualClock> clock{std::make_shared<ManualClock>(100, 1000)};
  ProviderManager provider;
  TargetCatalog target{healthCatalog()};
  SemanticCatalog semantic{target};
  DataManager data;
  WatchManager watches;
  MetricsManager metrics{data, *clock};
  std::shared_ptr<const CatalogView> catalog;
  EntityRef entity;
  std::unique_ptr<CollectionCoordinator> collection;

  explicit HealthRoundTrip(const MockHeartbeatClass heartbeat)
      : provider(std::make_unique<MockProvider>(
            MockProviderConfig{MockScenario::kSingleDevice, heartbeat,
                               TargetKind::kFpga},
            clock)) {
    ProviderInitOptions init;
    init.target = TargetKind::kFpga;
    init.deadline = clock->monotonicNow() + std::chrono::seconds(1);
    if (!provider.initialize(init).ok()) {
      throw std::runtime_error("mock provider initialization failed");
    }
    const ProviderDiscoveryResult discovered =
        provider.discover(clock->monotonicNow() + std::chrono::seconds(1));
    if (!discovered.call_status.ok()) {
      throw std::runtime_error("mock provider discovery failed");
    }
    const CatalogCommitResult committed =
        semantic.commit(discovered.descriptor);
    if (!committed.status.ok()) {
      throw std::runtime_error("semantic catalog commit failed");
    }
    catalog = semantic.snapshot();
    entity = catalog->entities().front().ref;
    if (!data.activateCatalog(catalog, target).ok() ||
        !watches.activateCatalog(catalog, target).ok() ||
        !metrics.activateCatalog(catalog, target).ok() ||
        !watches
             .setFirmwareHeartbeatBaseline(Nanoseconds{10}, Nanoseconds{1000})
             .ok()) {
      throw std::runtime_error("health managers activation failed");
    }
    collection = std::make_unique<CollectionCoordinator>(
        provider, data, clock, CollectionLimits{}, &watches, &metrics);
    if (!collection->activateCatalog(catalog).ok() ||
        !collection->applyWatchSnapshot(watches.snapshot()).ok()) {
      throw std::runtime_error("health collection activation failed");
    }
  }

  CollectionRunResult collect() {
    if (collection->plan()->jobs.size() != 1) {
      throw std::runtime_error("heartbeat plan is not singular");
    }
    return collection->collectJob(
        collection->plan()->jobs.front().id, clock->monotonicNow(),
        clock->monotonicNow() + std::chrono::seconds(1));
  }

  HealthQueryResult query() const {
    return metrics.queryHealth(HealthRequest{entity, kFirmwareHeartbeatHealthId,
                                             catalog->generation(), 0});
  }
};

HealthState collectState(const MockHeartbeatClass heartbeat) {
  HealthRoundTrip fixture(heartbeat);
  const CollectionRunResult collected = fixture.collect();
  if (!collected.status.ok() || !collected.evidence_commit.committed) {
    throw std::runtime_error("heartbeat collection failed");
  }
  const HealthQueryResult queried = fixture.query();
  if (!queried.item.has_value()) {
    throw std::runtime_error("heartbeat query returned no item");
  }
  return queried.item->state;
}

TEST(MockHealthRoundTripTest, MapsNormalizedMockHeartbeatClasses) {
  EXPECT_EQ(collectState(MockHeartbeatClass::kNormal), HealthState::kHealthy);
  EXPECT_EQ(collectState(MockHeartbeatClass::kWarning), HealthState::kWarning);
  EXPECT_EQ(collectState(MockHeartbeatClass::kFault), HealthState::kError);
}

TEST(MockHealthRoundTripTest,
     WatchCollectionEvidenceHealthAndFreshnessFormClosedLoop) {
  HealthRoundTrip fixture(MockHeartbeatClass::kNormal);
  EXPECT_EQ(fixture.target.metrics_status,
            MetricsCatalogStatus::kBlockedExternal);
  EXPECT_TRUE(fixture.target.metrics.empty());
  ASSERT_EQ(fixture.collection->plan()->jobs.size(), 1);
  EXPECT_EQ(fixture.collection->plan()->jobs.front().kind,
            ProviderDataKind::kHeartbeatEvidence);

  const CollectionRunResult first = fixture.collect();
  ASSERT_TRUE(first.status.ok());
  ASSERT_TRUE(first.evidence_commit.status.ok());
  EXPECT_TRUE(first.evidence_commit.committed);
  HealthQueryResult queried = fixture.query();
  ASSERT_TRUE(queried.status.ok());
  ASSERT_TRUE(queried.item.has_value());
  ASSERT_TRUE(queried.aggregate.has_value());
  EXPECT_EQ(queried.item->state, HealthState::kHealthy);
  EXPECT_EQ(queried.aggregate->state, queried.item->state);
  ASSERT_EQ(queried.item->evidence.size(), 1);
  EXPECT_EQ(queried.item->evidence.front().sequence_or_token, 1);

  fixture.clock->advance(Nanoseconds{101});
  ASSERT_TRUE(
      fixture.metrics.runFreshnessOnce(fixture.clock->monotonicNow()).ok());
  queried = fixture.query();
  EXPECT_EQ(queried.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  ASSERT_TRUE(queried.item.has_value());
  EXPECT_EQ(queried.item->state, HealthState::kUnknown);
  EXPECT_EQ(queried.item->code, StableHealthCode::kHeartbeatStale);

  fixture.clock->advance(Nanoseconds{9});
  const CollectionRunResult recovered = fixture.collect();
  ASSERT_TRUE(recovered.status.ok());
  EXPECT_TRUE(recovered.evidence_commit.committed);
  queried = fixture.query();
  ASSERT_TRUE(queried.status.ok());
  ASSERT_TRUE(queried.item.has_value());
  EXPECT_EQ(queried.item->state, HealthState::kHealthy);
  ASSERT_EQ(queried.item->evidence.size(), 1);
  EXPECT_EQ(queried.item->evidence.front().sequence_or_token, 2);
}

} // namespace
} // namespace pdcm::testkit
