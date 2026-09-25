#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "collection/collection_coordinator.hpp"
#include "collection/watch_manager.hpp"
#include "data/query_engine.hpp"
#include "pdcm/testkit/manual_clock.hpp"
#include "pdcm/testkit/mock_metric_ids.hpp"
#include "pdcm/testkit/mock_provider.hpp"
#include "provider/provider_manager.hpp"
#include "semantic/semantic_catalog.hpp"

namespace pdcm::testkit {
namespace {

MetricDescriptor collectionMetric(const std::uint32_t id,
                                  const char *const name) {
  MetricDescriptor metric;
  metric.id = MetricId{id};
  metric.name = name;
  metric.value_type = MetricValueKind::kUint64;
  metric.unit = "count";
  metric.scope = EntityKind::kDevice;
  metric.default_period_ns = 10;
  metric.min_period_ns = 1;
  metric.freshness_ns = 100;
  metric.supports_fpga = true;
  metric.requirement = RequirementLevel::kRequired;
  metric.semantic_version = 1;
  metric.provider_mapping_approved = true;
  return metric;
}

TargetCatalog collectionRoundTripCatalog(const bool include_failure) {
  TargetCatalog catalog = TargetCatalog::blocked(TargetKind::kFpga);
  catalog.metrics_status = MetricsCatalogStatus::kReady;
  catalog.metrics = {
      collectionMetric(kTestGaugeMetricId, "mock_gauge"),
      collectionMetric(kTestCounterMetricId, "mock_counter"),
  };
  if (include_failure) {
    catalog.metrics.push_back(collectionMetric(kTestPeriodicFailureMetricId,
                                               "mock_periodic_failure"));
  }
  return catalog;
}

struct CollectionRoundTrip {
  std::shared_ptr<ManualClock> clock{std::make_shared<ManualClock>(100, 1000)};
  ProviderManager provider;
  TargetCatalog target;
  SemanticCatalog semantic;
  DataManager data;
  WatchManager watches;
  std::shared_ptr<const CatalogView> catalog;
  EntityRef entity;

  explicit CollectionRoundTrip(const MockScenario scenario)
      : provider(std::make_unique<MockProvider>(
            MockProviderConfig{scenario, MockHeartbeatClass::kNormal,
                               TargetKind::kFpga},
            clock)),
        target(collectionRoundTripCatalog(scenario ==
                                          MockScenario::kPartialMetricFailure)),
        semantic(target) {
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
        !watches.activateCatalog(catalog, target).ok()) {
      throw std::runtime_error("collection catalogs failed to activate");
    }
  }

  std::shared_ptr<const WatchSnapshot>
  createWatch(std::vector<MetricId> metrics) {
    WatchRequirement request;
    request.catalog_generation = catalog->generation();
    request.entity = entity;
    request.metrics = std::move(metrics);
    request.period = Nanoseconds{10};
    request.freshness = Nanoseconds{100};
    request.retention = Nanoseconds{1000};
    const WatchCreateResult created = watches.create(
        WatchOwner{WatchOwnerKind::kSession, 1}, std::move(request));
    if (!created.status.ok()) {
      throw std::runtime_error("logical watch creation failed");
    }
    return watches.snapshot();
  }
};

TEST(MockCollectionRoundTripTest,
     WatchPlanProviderCommitAndQueryFormClosedLoop) {
  CollectionRoundTrip fixture(MockScenario::kSingleDevice);
  CollectionCoordinator coordinator(fixture.provider, fixture.data,
                                    fixture.clock);
  ASSERT_TRUE(coordinator.activateCatalog(fixture.catalog).ok());
  ASSERT_TRUE(
      coordinator
          .applyWatchSnapshot(fixture.createWatch(
              {MetricId{kTestCounterMetricId}, MetricId{kTestGaugeMetricId}}))
          .ok());
  ASSERT_EQ(coordinator.plan()->jobs.size(), 1U);

  const CollectionRunResult collected = coordinator.collectJob(
      coordinator.plan()->jobs.front().id, fixture.clock->monotonicNow(),
      fixture.clock->monotonicNow() + std::chrono::seconds(1));
  ASSERT_TRUE(collected.status.ok());
  EXPECT_EQ(collected.commit.committed_items, 2U);

  QueryEngine query(fixture.data, fixture.clock, &coordinator);
  QueryRequest request;
  request.catalog_generation = fixture.catalog->generation();
  request.selection = {
      {fixture.entity, MetricId{kTestCounterMetricId}},
      {fixture.entity, MetricId{kTestGaugeMetricId}},
  };
  const QueryResult result = query.execute(request);
  ASSERT_TRUE(result.status.ok());
  ASSERT_EQ(result.items.size(), 2U);
  EXPECT_EQ(result.items[0].metric.value, kTestGaugeMetricId);
  EXPECT_EQ(std::get<std::uint64_t>(*result.items[0].value), 10U);
  EXPECT_EQ(result.items[0].scheduled_monotonic_time_ns, 100);
  EXPECT_EQ(result.items[1].metric.value, kTestCounterMetricId);
  EXPECT_EQ(std::get<std::uint64_t>(*result.items[1].value), 100U);
}

TEST(MockCollectionRoundTripTest,
     PartialMetricFailureDoesNotBlockValidSibling) {
  CollectionRoundTrip fixture(MockScenario::kPartialMetricFailure);
  CollectionCoordinator coordinator(fixture.provider, fixture.data,
                                    fixture.clock);
  ASSERT_TRUE(coordinator.activateCatalog(fixture.catalog).ok());
  ASSERT_TRUE(coordinator
                  .applyWatchSnapshot(fixture.createWatch(
                      {MetricId{kTestGaugeMetricId},
                       MetricId{kTestPeriodicFailureMetricId}}))
                  .ok());

  const CollectionRunResult collected = coordinator.collectJob(
      coordinator.plan()->jobs.front().id, fixture.clock->monotonicNow(),
      fixture.clock->monotonicNow() + std::chrono::seconds(1));
  EXPECT_EQ(collected.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  EXPECT_EQ(collected.commit.committed_items, 2U);

  const DataReadResult read = fixture.data.readLatest(
      {{fixture.entity, MetricId{kTestGaugeMetricId}},
       {fixture.entity, MetricId{kTestPeriodicFailureMetricId}}},
      true, fixture.clock->monotonicNow().time_since_epoch().count(),
      fixture.catalog->generation());
  ASSERT_EQ(read.items.size(), 2U);
  EXPECT_EQ(read.items[0].status, ObservationStatus::kValid);
  EXPECT_EQ(read.items[1].status, ObservationStatus::kError);
  EXPECT_EQ(read.items[1].error.native_code, 2001);
  EXPECT_TRUE(read.items[1].error.retryable);
}

} // namespace
} // namespace pdcm::testkit
