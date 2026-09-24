#include "collection/watch_manager.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace pdcm {
namespace {

TargetCatalog watchCatalog() {
  TargetCatalog catalog = TargetCatalog::blocked(TargetKind::kFpga);
  catalog.metrics_status = MetricsCatalogStatus::kReady;
  catalog.metrics.clear();
  for (std::uint32_t id = 1; id <= 2; ++id) {
    MetricDescriptor metric;
    metric.id = MetricId{id};
    metric.name = "watch_metric_" + std::to_string(id);
    metric.value_type = MetricValueKind::kUint64;
    metric.unit = "count";
    metric.scope = EntityKind::kDevice;
    metric.default_period_ns = 100;
    metric.min_period_ns = 10;
    metric.freshness_ns = 500;
    metric.supports_fpga = true;
    metric.requirement = RequirementLevel::kRequired;
    metric.semantic_version = 1;
    metric.provider_mapping_approved = true;
    catalog.metrics.push_back(metric);
  }
  return catalog;
}

ProviderDescriptor watchProvider(const std::uint32_t devices = 1) {
  ProviderDescriptor descriptor;
  descriptor.provider_version = "watch-provider";
  descriptor.target = TargetKind::kFpga;
  descriptor.state = ProviderState::kReady;
  descriptor.detected_device_count = devices;
  for (std::uint32_t index = 0; index < devices; ++index) {
    ProviderEntity entity;
    entity.stable_native_id = "device-" + std::to_string(index);
    entity.pci_bdf = "0000:01:00." + std::to_string(index);
    entity.pdrv_version = "watch-pdrv";
    entity.incarnation = "boot-" + std::to_string(index);
    entity.state = ProviderEntityState::kReady;
    entity.manageable = true;
    descriptor.entities.push_back(std::move(entity));
  }
  for (std::uint32_t id = 1; id <= 2; ++id) {
    descriptor.capabilities.push_back(
        ProviderCapability{ProviderDataKind::kMetric, id, true, {}});
  }
  return descriptor;
}

struct ActiveWatchCatalog {
  TargetCatalog target{watchCatalog()};
  SemanticCatalog semantic{target};
  std::shared_ptr<const CatalogView> view;
  EntityRef entity;

  explicit ActiveWatchCatalog(const std::uint32_t devices = 1) {
    const CatalogCommitResult result = semantic.commit(watchProvider(devices));
    if (!result.committed) {
      throw std::runtime_error("watch catalog commit failed");
    }
    view = semantic.snapshot();
    if (!view->entities().empty()) {
      entity = view->entities().front().ref;
    }
  }
};

WatchRequirement requirement(const ActiveWatchCatalog &catalog,
                             std::vector<MetricId> metrics,
                             const std::int64_t period = 100) {
  WatchRequirement request;
  request.catalog_generation = catalog.view->generation();
  request.entity = catalog.entity;
  request.metrics = std::move(metrics);
  request.period = Nanoseconds{period};
  request.freshness = Nanoseconds{period * 2};
  request.retention = Nanoseconds{period * 4};
  return request;
}

TEST(WatchManagerTest, MergesOwnersDeterministicallyAndKeepsPhysicalWatch) {
  ActiveWatchCatalog catalog;
  WatchManager manager;
  ASSERT_TRUE(manager.activateCatalog(catalog.view, catalog.target).ok());

  WatchRequirement first = requirement(catalog, {MetricId{1}}, 100);
  first.freshness = Nanoseconds{300};
  first.retention = Nanoseconds{400};
  first.priority = WatchPriority::kNormal;
  first.sample_limit = 2;
  const WatchCreateResult one =
      manager.create(WatchOwner{WatchOwnerKind::kSession, 1}, first);
  ASSERT_TRUE(one.status.ok());

  WatchRequirement second = requirement(catalog, {MetricId{1}}, 50);
  second.freshness = Nanoseconds{100};
  second.retention = Nanoseconds{1000};
  second.priority = WatchPriority::kHigh;
  second.sample_limit = 9;
  const WatchCreateResult two =
      manager.create(WatchOwner{WatchOwnerKind::kSession, 2}, second);
  ASSERT_TRUE(two.status.ok());

  std::shared_ptr<const WatchSnapshot> snapshot = manager.snapshot();
  ASSERT_EQ(snapshot->logical_watches.size(), 2U);
  ASSERT_EQ(snapshot->effective_watches.size(), 1U);
  EXPECT_EQ(snapshot->effective_watches.front().period, Nanoseconds{50});
  EXPECT_EQ(snapshot->effective_watches.front().freshness, Nanoseconds{100});
  EXPECT_EQ(snapshot->effective_watches.front().retention, Nanoseconds{1000});
  EXPECT_EQ(snapshot->effective_watches.front().priority, WatchPriority::kHigh);
  EXPECT_EQ(snapshot->effective_watches.front().logical_watches.size(), 2U);

  ASSERT_TRUE(
      manager.destroy(WatchOwner{WatchOwnerKind::kSession, 1}, one.watch_id)
          .ok());
  snapshot = manager.snapshot();
  ASSERT_EQ(snapshot->logical_watches.size(), 1U);
  ASSERT_EQ(snapshot->effective_watches.size(), 1U);
  EXPECT_EQ(snapshot->effective_watches.front().period, Nanoseconds{50});
  EXPECT_EQ(snapshot->logical_watches.front().requirement.sample_limit, 9U);
}

TEST(WatchManagerTest, UnsupportedSelectionIsAtomicUnlessPartialAllowed) {
  ActiveWatchCatalog catalog;
  WatchManager manager;
  ASSERT_TRUE(manager.activateCatalog(catalog.view, catalog.target).ok());

  WatchRequirement atomic = requirement(catalog, {MetricId{1}, MetricId{99}});
  const WatchCreateResult rejected =
      manager.create(WatchOwner{WatchOwnerKind::kSession, 1}, atomic);
  EXPECT_EQ(rejected.status.code(), PDCM_STATUS_UNSUPPORTED);
  EXPECT_TRUE(manager.snapshot()->logical_watches.empty());

  WatchRequirement partial = atomic;
  partial.allow_partial = true;
  const WatchCreateResult accepted =
      manager.create(WatchOwner{WatchOwnerKind::kSession, 1}, partial);
  EXPECT_EQ(accepted.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  ASSERT_EQ(accepted.unsupported_metrics.size(), 1U);
  const auto snapshot = manager.snapshot();
  ASSERT_EQ(snapshot->logical_watches.size(), 1U);
  ASSERT_EQ(snapshot->logical_watches.front().requirement.metrics.size(), 1U);
  EXPECT_EQ(snapshot->logical_watches.front().requirement.metrics.front(),
            MetricId{1});
}

TEST(WatchManagerTest, CapacityFailurePreservesPublishedSnapshot) {
  ActiveWatchCatalog catalog;
  WatchLimits limits;
  limits.max_logical_watches = 1;
  WatchManager manager(limits);
  ASSERT_TRUE(manager.activateCatalog(catalog.view, catalog.target).ok());
  const WatchOwner owner{WatchOwnerKind::kSession, 1};
  ASSERT_TRUE(
      manager.create(owner, requirement(catalog, {MetricId{1}})).status.ok());
  const std::uint64_t version = manager.snapshot()->version;

  const WatchCreateResult rejected =
      manager.create(owner, requirement(catalog, {MetricId{2}}));
  EXPECT_EQ(rejected.status.code(), PDCM_STATUS_RESOURCE_EXHAUSTED);
  EXPECT_EQ(manager.snapshot()->version, version);
  EXPECT_EQ(manager.snapshot()->logical_watches.size(), 1U);
}

TEST(WatchManagerTest, OwnerRemovalDoesNotRemoveOtherOwnersRequirement) {
  ActiveWatchCatalog catalog;
  WatchManager manager;
  ASSERT_TRUE(manager.activateCatalog(catalog.view, catalog.target).ok());
  const WatchOwner session{WatchOwnerKind::kSession, 7};
  const WatchOwner heartbeat{WatchOwnerKind::kFirmwareHeartbeat, 0};
  ASSERT_TRUE(
      manager.create(session, requirement(catalog, {MetricId{1}})).status.ok());
  ASSERT_TRUE(manager.create(heartbeat, requirement(catalog, {MetricId{1}}))
                  .status.ok());

  ASSERT_TRUE(manager.removeOwner(session).ok());
  const auto snapshot = manager.snapshot();
  ASSERT_EQ(snapshot->logical_watches.size(), 1U);
  EXPECT_EQ(snapshot->logical_watches.front().owner.kind,
            WatchOwnerKind::kFirmwareHeartbeat);
  EXPECT_EQ(snapshot->effective_watches.size(), 1U);
}

TEST(WatchManagerTest, CatalogGenerationChangeRemovesStaleRequirements) {
  ActiveWatchCatalog catalog;
  WatchManager manager;
  ASSERT_TRUE(manager.activateCatalog(catalog.view, catalog.target).ok());
  ASSERT_TRUE(manager
                  .create(WatchOwner{WatchOwnerKind::kSession, 1},
                          requirement(catalog, {MetricId{1}}))
                  .status.ok());

  const CatalogCommitResult changed = catalog.semantic.commit(watchProvider(0));
  ASSERT_TRUE(changed.status.ok());
  catalog.view = catalog.semantic.snapshot();
  ASSERT_TRUE(manager.activateCatalog(catalog.view, catalog.target).ok());

  EXPECT_TRUE(manager.snapshot()->logical_watches.empty());
  EXPECT_TRUE(manager.snapshot()->effective_watches.empty());
  EXPECT_EQ(manager.snapshot()->catalog_generation, catalog.view->generation());
}

TEST(WatchManagerTest, MultipleDevicesNeverCreateCollectionWatch) {
  ActiveWatchCatalog catalog(2);
  WatchManager manager;
  ASSERT_TRUE(manager.activateCatalog(catalog.view, catalog.target).ok());
  WatchRequirement request;
  request.catalog_generation = catalog.view->generation();
  request.entity = catalog.entity;
  request.metrics = {MetricId{1}};
  request.period = Nanoseconds{100};
  request.freshness = Nanoseconds{200};
  request.retention = Nanoseconds{400};

  const WatchCreateResult result =
      manager.create(WatchOwner{WatchOwnerKind::kSession, 1}, request);
  EXPECT_EQ(result.status.code(), PDCM_STATUS_UNSUPPORTED);
  EXPECT_TRUE(manager.snapshot()->effective_watches.empty());
}

TEST(WatchManagerTest, SampleLimitCountsEachScheduledPeriodExactlyOnce) {
  ActiveWatchCatalog catalog;
  WatchManager manager;
  ASSERT_TRUE(manager.activateCatalog(catalog.view, catalog.target).ok());

  WatchRequirement limited = requirement(catalog, {MetricId{1}});
  limited.sample_limit = 2;
  const WatchCreateResult first =
      manager.create(WatchOwner{WatchOwnerKind::kSession, 1}, limited);
  ASSERT_TRUE(first.status.ok());

  WatchRequirement unlimited = requirement(catalog, {MetricId{1}});
  const WatchCreateResult second =
      manager.create(WatchOwner{WatchOwnerKind::kSession, 2}, unlimited);
  ASSERT_TRUE(second.status.ok());

  ASSERT_TRUE(manager
                  .recordSamples({first.watch_id, second.watch_id},
                                 MonotonicTime{Nanoseconds{100}},
                                 Nanoseconds{100})
                  .ok());
  ASSERT_TRUE(manager
                  .recordSamples({first.watch_id, second.watch_id},
                                 MonotonicTime{Nanoseconds{100}},
                                 Nanoseconds{100})
                  .ok());
  EXPECT_EQ(manager.snapshot()->logical_watches.size(), 2U);

  ASSERT_TRUE(manager
                  .recordSamples({first.watch_id, second.watch_id},
                                 MonotonicTime{Nanoseconds{200}},
                                 Nanoseconds{100})
                  .ok());
  const auto snapshot = manager.snapshot();
  ASSERT_EQ(snapshot->logical_watches.size(), 1U);
  EXPECT_EQ(snapshot->logical_watches.front().id, second.watch_id);
  ASSERT_EQ(snapshot->effective_watches.size(), 1U);
  EXPECT_EQ(snapshot->effective_watches.front().logical_watches,
            std::vector<WatchId>{second.watch_id});
}
} // namespace
} // namespace pdcm
