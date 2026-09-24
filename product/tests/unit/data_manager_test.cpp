#include "data/data_manager.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace pdcm {
namespace {

TargetCatalog testCatalog() {
  TargetCatalog catalog = TargetCatalog::blocked(TargetKind::kFpga);
  catalog.metrics_status = MetricsCatalogStatus::kReady;

  MetricDescriptor metric;
  metric.id = MetricId{1};
  metric.name = "test_counter";
  metric.value_type = MetricValueKind::kUint64;
  metric.unit = "count";
  metric.temporality = MetricTemporality::kCumulativeCounter;
  metric.default_period_ns = 10;
  metric.min_period_ns = 1;
  metric.freshness_ns = 100;
  metric.supports_fpga = true;
  metric.requirement = RequirementLevel::kConditional;
  metric.semantic_version = 1;
  catalog.metrics.push_back(metric);
  return catalog;
}

ProviderDescriptor providerDescriptor(const bool present = true) {
  ProviderDescriptor descriptor;
  descriptor.provider_version = "data-test-provider";
  descriptor.target = TargetKind::kFpga;
  descriptor.state = ProviderState::kReady;
  if (present) {
    descriptor.detected_device_count = 1;
    ProviderEntity entity;
    entity.stable_native_id = "data-device";
    entity.pci_bdf = "0000:01:00.0";
    entity.pdrv_version = "data-pdrv";
    entity.incarnation = "boot-1";
    entity.state = ProviderEntityState::kReady;
    entity.manageable = true;
    descriptor.entities.push_back(std::move(entity));
  }
  return descriptor;
}

struct ActiveCatalog {
  TargetCatalog target;
  std::unique_ptr<SemanticCatalog> semantic;
  std::shared_ptr<const CatalogView> view;
  EntityRef entity;
};

ActiveCatalog activeCatalog() {
  ActiveCatalog active;
  active.target = testCatalog();
  active.semantic = std::make_unique<SemanticCatalog>(active.target);
  const CatalogCommitResult committed =
      active.semantic->commit(providerDescriptor());
  if (!committed.committed) {
    throw std::runtime_error("test catalog commit failed");
  }
  active.view = active.semantic->snapshot();
  active.entity = active.view->entities().front().ref;
  return active;
}

Observation validObservation(const EntityRef entity, const std::uint64_t value,
                             const std::int64_t time,
                             const std::uint64_t catalog_generation = 1) {
  Observation observation;
  observation.entity = entity;
  observation.metric = MetricId{1};
  observation.value = value;
  observation.status = ObservationStatus::kValid;
  observation.metric_semantic_version = 1;
  observation.observed_monotonic_time_ns = time;
  observation.observed_wall_time_ns = time + 1000;
  observation.catalog_generation = catalog_generation;
  observation.source.provider = "mock-provider";
  observation.source.native_source = "mock-counter";
  return observation;
}

Observation failedObservation(const EntityRef entity, const std::int64_t time,
                              const std::uint64_t catalog_generation = 1) {
  Observation observation =
      validObservation(entity, 0, time, catalog_generation);
  observation.value.reset();
  observation.status = ObservationStatus::kError;
  observation.error.status =
      Status(PDCM_STATUS_UNAVAILABLE, "scripted read failure");
  observation.error.native_code = 77;
  observation.error.retryable = true;
  return observation;
}

TEST(DataManagerTest, PreservesLatestFailureAndSynthesizesStaleValue) {
  ActiveCatalog active = activeCatalog();
  DataManager data;
  ASSERT_TRUE(data.activateCatalog(active.view, active.target).ok());

  const DataCommitResult first =
      data.commit({validObservation(active.entity, 42, 10)});
  ASSERT_TRUE(first.status.ok());
  EXPECT_EQ(first.commit_epoch, 1);

  const DataCommitResult second =
      data.commit({failedObservation(active.entity, 20)});
  ASSERT_EQ(second.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  EXPECT_EQ(second.commit_epoch, 2);

  const DataKey key{active.entity, MetricId{1}};
  const DataReadResult latest = data.readLatest({key}, false, 30, 1);
  ASSERT_EQ(latest.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  ASSERT_EQ(latest.items.size(), 1);
  EXPECT_EQ(latest.items.front().status, ObservationStatus::kError);
  EXPECT_FALSE(latest.items.front().value.has_value());

  const DataReadResult stale = data.readLatest({key}, true, 30, 1);
  ASSERT_EQ(stale.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  ASSERT_EQ(stale.items.size(), 1);
  EXPECT_EQ(stale.items.front().status, ObservationStatus::kStale);
  ASSERT_TRUE(stale.items.front().value.has_value());
  EXPECT_EQ(std::get<std::uint64_t>(*stale.items.front().value), 42);
  ASSERT_TRUE(stale.items.front().stale_age_ns.has_value());
  EXPECT_EQ(*stale.items.front().stale_age_ns, 20);
  ASSERT_TRUE(stale.items.front().latest_failure.has_value());
  EXPECT_EQ(stale.items.front().latest_failure->status.code(),
            PDCM_STATUS_UNAVAILABLE);
  EXPECT_TRUE(validateObservation(stale.items.front()).ok());

  const DataHistoryResult history = data.history(key);
  ASSERT_TRUE(history.status.ok());
  ASSERT_EQ(history.samples.size(), 1);
  EXPECT_EQ(std::get<std::uint64_t>(*history.samples.front().value), 42);
}

TEST(DataManagerTest, RejectsMalformedBatchAtomically) {
  ActiveCatalog active = activeCatalog();
  DataManager data;
  ASSERT_TRUE(data.activateCatalog(active.view, active.target).ok());

  Observation valid = validObservation(active.entity, 1, 10);
  Observation wrong_type = validObservation(active.entity, 2, 10);
  wrong_type.metric = MetricId{2};

  const DataCommitResult duplicate = data.commit({valid, valid});
  EXPECT_EQ(duplicate.status.code(), PDCM_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(data.commitEpoch(), 0);
  EXPECT_EQ(data.keyCount(), 0);

  const DataCommitResult unknown = data.commit({valid, wrong_type});
  EXPECT_EQ(unknown.status.code(), PDCM_STATUS_UNSUPPORTED);
  EXPECT_EQ(data.commitEpoch(), 0);
  EXPECT_EQ(data.keyCount(), 0);

  valid.value = std::string("not-a-counter");
  const DataCommitResult invalid_type = data.commit({valid});
  EXPECT_EQ(invalid_type.status.code(), PDCM_STATUS_INVALID_ARGUMENT);
  EXPECT_EQ(data.commitEpoch(), 0);
}

TEST(DataManagerTest, EnforcesHistoryAndByteLimits) {
  ActiveCatalog active = activeCatalog();
  DataStoreLimits limits;
  limits.max_history_samples_per_key = 2;
  limits.max_history_duration_ns = 1000;
  DataManager data(limits);
  ASSERT_TRUE(data.activateCatalog(active.view, active.target).ok());

  ASSERT_TRUE(
      data.commit({validObservation(active.entity, 1, 10)}).status.ok());
  ASSERT_TRUE(
      data.commit({validObservation(active.entity, 2, 20)}).status.ok());
  ASSERT_TRUE(
      data.commit({validObservation(active.entity, 3, 30)}).status.ok());

  const DataHistoryResult history =
      data.history(DataKey{active.entity, MetricId{1}});
  ASSERT_TRUE(history.status.ok());
  ASSERT_EQ(history.samples.size(), 2);
  EXPECT_EQ(std::get<std::uint64_t>(*history.samples[0].value), 2);
  EXPECT_EQ(std::get<std::uint64_t>(*history.samples[1].value), 3);
  EXPECT_LE(data.storedBytes(), limits.max_total_bytes);

  DataStoreLimits tiny;
  tiny.max_total_bytes = 128;
  tiny.max_value_bytes = 64;
  DataManager constrained(tiny);
  ASSERT_TRUE(constrained.activateCatalog(active.view, active.target).ok());
  EXPECT_EQ(constrained.commit({validObservation(active.entity, 1, 10)})
                .status.code(),
            PDCM_STATUS_RESOURCE_EXHAUSTED);
  EXPECT_EQ(constrained.keyCount(), 0);
}

TEST(DataManagerTest, RejectsOldGenerationAndCreatesTombstone) {
  ActiveCatalog active = activeCatalog();
  DataManager data;
  ASSERT_TRUE(data.activateCatalog(active.view, active.target).ok());
  ASSERT_TRUE(
      data.commit({validObservation(active.entity, 1, 10)}).status.ok());

  const CatalogCommitResult removed =
      active.semantic->commit(providerDescriptor(false));
  ASSERT_TRUE(removed.committed);
  ASSERT_TRUE(
      data.activateCatalog(active.semantic->snapshot(), active.target).ok());

  EXPECT_EQ(data.keyCount(), 0);
  const std::vector<EntityTombstone> tombstones = data.tombstones();
  ASSERT_EQ(tombstones.size(), 1);
  EXPECT_EQ(tombstones.front().entity, active.entity);

  const DataCommitResult stale =
      data.commit({validObservation(active.entity, 2, 20, 1)});
  EXPECT_EQ(stale.status.code(), PDCM_STATUS_STALE_GENERATION);
}

TEST(DataManagerTest, ConcurrentReadersObserveCommittedSnapshots) {
  ActiveCatalog active = activeCatalog();
  DataManager data;
  ASSERT_TRUE(data.activateCatalog(active.view, active.target).ok());
  ASSERT_TRUE(data.commit({validObservation(active.entity, 0, 0)}).status.ok());

  const DataKey key{active.entity, MetricId{1}};
  std::atomic<bool> writer_done{false};
  std::atomic<bool> reader_failed{false};

  std::thread writer([&] {
    for (std::uint64_t value = 1; value <= 100; ++value) {
      const DataCommitResult committed = data.commit({validObservation(
          active.entity, value, static_cast<std::int64_t>(value))});
      if (!committed.status.ok()) {
        reader_failed.store(true);
        break;
      }
    }
    writer_done.store(true);
  });
  std::thread reader([&] {
    while (!writer_done.load()) {
      const DataReadResult snapshot = data.readLatest({key}, false, 1000, 1);
      if (!snapshot.status.ok() || snapshot.items.size() != 1 ||
          snapshot.items.front().commit_epoch >
              snapshot.observed_commit_epoch) {
        reader_failed.store(true);
        break;
      }
    }
  });

  writer.join();
  reader.join();
  EXPECT_FALSE(reader_failed.load());
  EXPECT_EQ(data.commitEpoch(), 101);
}

TEST(DataManagerTest, WaitForEpochHonorsDeadline) {
  ActiveCatalog active = activeCatalog();
  DataManager data;
  ASSERT_TRUE(data.activateCatalog(active.view, active.target).ok());

  const MonotonicTime expired = std::chrono::time_point_cast<Nanoseconds>(
      std::chrono::steady_clock::now());
  EXPECT_EQ(data.waitForEpoch(1, expired).code(), PDCM_STATUS_TIMEOUT);

  ASSERT_TRUE(
      data.commit({validObservation(active.entity, 1, 10)}).status.ok());
  EXPECT_TRUE(data.waitForEpoch(1, expired).ok());
}

} // namespace
} // namespace pdcm
#include <stdexcept>
