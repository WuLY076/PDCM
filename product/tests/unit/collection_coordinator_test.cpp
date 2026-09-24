#include "collection/collection_coordinator.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace pdcm {
namespace {

class TestClock final : public Clock {
public:
  TestClock(const std::int64_t monotonic_ns,
            const std::int64_t wall_ns) noexcept
      : monotonic_ns_(monotonic_ns), wall_ns_(wall_ns) {}

  [[nodiscard]] MonotonicTime monotonicNow() const noexcept override {
    return MonotonicTime{Nanoseconds{monotonic_ns_}};
  }

  [[nodiscard]] std::int64_t wallTimeNanoseconds() const noexcept override {
    return wall_ns_;
  }

private:
  std::int64_t monotonic_ns_;
  std::int64_t wall_ns_;
};

class ScriptedProvider final : public Provider {
public:
  using ReadScript =
      std::function<ProviderReadResult(const ProviderReadRequest &)>;

  explicit ScriptedProvider(ReadScript script) : script_(std::move(script)) {}

  Status initialize(const ProviderInitOptions &) override {
    state_ = ProviderState::kReady;
    return Status::success();
  }

  void shutdown() noexcept override { state_ = ProviderState::kShutdown; }

  [[nodiscard]] ProviderState state() const noexcept override { return state_; }

  [[nodiscard]] ProviderConcurrency concurrency() const noexcept override {
    return ProviderConcurrency{1, false};
  }

  ProviderDiscoveryResult discover(MonotonicTime) override {
    ProviderDiscoveryResult result;
    result.call_status = Status::success();
    return result;
  }

  ProviderReadResult batchRead(const ProviderReadRequest &request) override {
    requests.push_back(request);
    return script_(request);
  }

  ReadScript script_;
  std::vector<ProviderReadRequest> requests;

private:
  ProviderState state_{ProviderState::kUninitialized};
};

TargetCatalog collectionCatalog() {
  TargetCatalog catalog = TargetCatalog::blocked(TargetKind::kFpga);
  catalog.metrics_status = MetricsCatalogStatus::kReady;
  for (std::uint32_t id = 1; id <= 2; ++id) {
    MetricDescriptor metric;
    metric.id = MetricId{id};
    metric.name = "collection_metric_" + std::to_string(id);
    metric.value_type = MetricValueKind::kUint64;
    metric.unit = "count";
    metric.scope = EntityKind::kDevice;
    metric.default_period_ns = 100;
    metric.min_period_ns = 10;
    metric.freshness_ns = 1000;
    metric.supports_fpga = true;
    metric.requirement = RequirementLevel::kRequired;
    metric.semantic_version = id;
    metric.provider_mapping_approved = true;
    catalog.metrics.push_back(metric);
  }
  return catalog;
}

ProviderDescriptor collectionProvider() {
  ProviderDescriptor descriptor;
  descriptor.provider_version = "collection-provider";
  descriptor.target = TargetKind::kFpga;
  descriptor.state = ProviderState::kReady;
  descriptor.detected_device_count = 1;

  ProviderEntity entity;
  entity.stable_native_id = "device-0";
  entity.pci_bdf = "0000:01:00.0";
  entity.pdrv_version = "pdrv-1";
  entity.incarnation = "boot-1";
  entity.state = ProviderEntityState::kReady;
  entity.manageable = true;
  descriptor.entities.push_back(entity);

  for (std::uint32_t id = 1; id <= 2; ++id) {
    descriptor.capabilities.push_back(
        ProviderCapability{ProviderDataKind::kMetric, id, true, {}});
  }
  return descriptor;
}

std::unique_ptr<Provider>
makeScriptedProvider(ScriptedProvider *&output,
                     ScriptedProvider::ReadScript script) {
  auto provider = std::make_unique<ScriptedProvider>(std::move(script));
  output = provider.get();
  return provider;
}

struct CollectionEnvironment {
  std::shared_ptr<TestClock> clock = std::make_shared<TestClock>(150, 10000);
  TargetCatalog target{collectionCatalog()};
  SemanticCatalog semantic{target};
  DataManager data;
  ScriptedProvider *scripted{nullptr};
  ProviderManager provider;
  WatchManager watches;
  std::shared_ptr<const CatalogView> view;
  EntityRef entity;

  explicit CollectionEnvironment(ScriptedProvider::ReadScript script)
      : provider(makeScriptedProvider(scripted, std::move(script))) {
    EXPECT_TRUE(provider
                    .initialize(ProviderInitOptions{
                        TargetKind::kFpga, MonotonicTime{Nanoseconds{1000}}})
                    .ok());
    const CatalogCommitResult committed = semantic.commit(collectionProvider());
    EXPECT_TRUE(committed.status.ok());
    view = semantic.snapshot();
    if (view->entities().size() != 1U) {
      throw std::runtime_error("collection catalog activation failed");
    }
    entity = view->entities().front().ref;
    EXPECT_TRUE(data.activateCatalog(view, target).ok());
    EXPECT_TRUE(watches.activateCatalog(view, target).ok());
  }

  std::shared_ptr<const WatchSnapshot> watchBoth() {
    WatchRequirement requirement;
    requirement.catalog_generation = view->generation();
    requirement.entity = entity;
    requirement.metrics = {MetricId{2}, MetricId{1}};
    requirement.period = Nanoseconds{100};
    requirement.freshness = Nanoseconds{200};
    requirement.retention = Nanoseconds{400};
    const WatchCreateResult created = watches.create(
        WatchOwner{WatchOwnerKind::kSession, 1}, std::move(requirement));
    EXPECT_TRUE(created.status.ok());
    return watches.snapshot();
  }
};

ProviderReadItemResult validItem(const ProviderReadItem &item,
                                 const std::uint64_t value) {
  ProviderReadItemResult result;
  result.item = item;
  result.status = ObservationStatus::kValid;
  result.value = value;
  result.source_sample_time_ns = 140;
  result.native_source = "scripted-native";
  return result;
}

TEST(CollectionCoordinatorTest, CompilesDeterministicBoundedPlanAtomically) {
  CollectionEnvironment environment([](const ProviderReadRequest &) {
    ProviderReadResult result;
    result.call_status = Status::success();
    return result;
  });
  CollectionLimits limits;
  limits.max_batch_items = 1;
  CollectionCoordinator coordinator(environment.provider, environment.data,
                                    environment.clock, limits);

  ASSERT_TRUE(coordinator.applyWatchSnapshot(environment.watchBoth()).ok());
  const std::shared_ptr<const CollectionPlan> plan = coordinator.plan();
  ASSERT_EQ(plan->jobs.size(), 2U);
  EXPECT_EQ(plan->version, 1U);
  EXPECT_EQ(plan->catalog_generation, environment.view->generation());
  EXPECT_EQ(plan->jobs[0].id, 1U);
  EXPECT_EQ(plan->jobs[0].items.front().key.data_id, 1U);
  EXPECT_EQ(plan->jobs[1].items.front().key.data_id, 2U);

  CollectionLimits too_small;
  too_small.max_plan_jobs = 1;
  too_small.max_batch_items = 1;
  CollectionCoordinator rejected(environment.provider, environment.data,
                                 environment.clock, too_small);
  const Status status =
      rejected.applyWatchSnapshot(environment.watches.snapshot());
  EXPECT_EQ(status.code(), PDCM_STATUS_RESOURCE_EXHAUSTED);
  EXPECT_EQ(rejected.plan()->version, 0U);
  EXPECT_TRUE(rejected.plan()->jobs.empty());
}

TEST(CollectionCoordinatorTest, NormalizesValidAndMissingItemsInOneCommit) {
  CollectionEnvironment environment([](const ProviderReadRequest &request) {
    ProviderReadResult result;
    result.call_status = Status::success();
    result.items.push_back(validItem(request.items.front(), 77));
    return result;
  });
  CollectionCoordinator coordinator(environment.provider, environment.data,
                                    environment.clock);
  ASSERT_TRUE(coordinator.applyWatchSnapshot(environment.watchBoth()).ok());

  const CollectionRunResult run = coordinator.collectJob(
      1, MonotonicTime{Nanoseconds{100}}, MonotonicTime{Nanoseconds{300}});
  EXPECT_EQ(run.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  EXPECT_EQ(run.normalized_items, 2U);
  EXPECT_EQ(run.commit.committed_items, 2U);
  ASSERT_EQ(environment.scripted->requests.size(), 1U);
  EXPECT_EQ(environment.scripted->requests.front().plan_generation,
            coordinator.plan()->version);

  const DataReadResult read = environment.data.readLatest(
      {{environment.entity, MetricId{1}}, {environment.entity, MetricId{2}}},
      true, 150, environment.view->generation());
  ASSERT_EQ(read.items.size(), 2U);
  EXPECT_EQ(read.items[0].status, ObservationStatus::kValid);
  EXPECT_EQ(read.items[0].scheduled_monotonic_time_ns, 100);
  EXPECT_EQ(read.items[0].source_sample_time_ns, 140);
  EXPECT_EQ(read.items[0].source.provider, "collection-provider");
  EXPECT_EQ(read.items[1].status, ObservationStatus::kError);
  EXPECT_EQ(read.items[1].error.status.code(), PDCM_STATUS_INTERNAL);
  EXPECT_FALSE(read.items[1].value.has_value());
}

TEST(CollectionCoordinatorTest, DiscardsDuplicateAndUnknownProviderItems) {
  CollectionEnvironment environment([](const ProviderReadRequest &request) {
    ProviderReadResult result;
    result.call_status = Status::success();
    result.items.push_back(validItem(request.items[0], 11));
    result.items.push_back(validItem(request.items[0], 12));
    result.items.push_back(validItem(request.items[1], 22));
    ProviderReadItem unknown = request.items[1];
    unknown.data_id = 99;
    result.items.push_back(validItem(unknown, 99));
    return result;
  });
  CollectionCoordinator coordinator(environment.provider, environment.data,
                                    environment.clock);
  ASSERT_TRUE(coordinator.applyWatchSnapshot(environment.watchBoth()).ok());

  const CollectionRunResult run = coordinator.collectJob(
      1, MonotonicTime{Nanoseconds{100}}, MonotonicTime{Nanoseconds{300}});
  EXPECT_EQ(run.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  EXPECT_EQ(run.contract_violations, 2U);
  EXPECT_EQ(run.normalized_items, 1U);
  EXPECT_EQ(run.commit.committed_items, 1U);

  const DataReadResult read =
      environment.data.readLatest({{environment.entity, MetricId{2}}}, true,
                                  150, environment.view->generation());
  ASSERT_EQ(read.items.size(), 1U);
  EXPECT_EQ(read.items.front().status, ObservationStatus::kValid);
  ASSERT_TRUE(read.items.front().value.has_value());
  EXPECT_EQ(std::get<std::uint64_t>(*read.items.front().value), 22U);
}

TEST(CollectionCoordinatorTest, MalformedValueDoesNotPoisonValidSibling) {
  CollectionEnvironment environment([](const ProviderReadRequest &request) {
    ProviderReadResult result;
    result.call_status = Status::success();
    ProviderReadItemResult malformed = validItem(request.items[0], 11);
    malformed.value = std::string{"wrong-type"};
    result.items.push_back(std::move(malformed));
    result.items.push_back(validItem(request.items[1], 22));
    return result;
  });
  CollectionCoordinator coordinator(environment.provider, environment.data,
                                    environment.clock);
  ASSERT_TRUE(coordinator.applyWatchSnapshot(environment.watchBoth()).ok());

  const CollectionRunResult run = coordinator.collectJob(
      1, MonotonicTime{Nanoseconds{100}}, MonotonicTime{Nanoseconds{300}});
  EXPECT_EQ(run.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  EXPECT_EQ(run.contract_violations, 1U);
  EXPECT_EQ(run.normalized_items, 1U);
  EXPECT_EQ(run.commit.committed_items, 1U);

  const DataReadResult read =
      environment.data.readLatest({{environment.entity, MetricId{2}}}, true,
                                  150, environment.view->generation());
  ASSERT_EQ(read.items.size(), 1U);
  EXPECT_EQ(read.items.front().status, ObservationStatus::kValid);
}
TEST(CollectionCoordinatorTest, SingleItemFailureDoesNotBlockValidSibling) {
  CollectionEnvironment environment([](const ProviderReadRequest &request) {
    ProviderReadResult result;
    result.call_status =
        Status(PDCM_STATUS_PARTIAL_RESULT, "one scripted failure");
    ProviderReadItemResult failed;
    failed.item = request.items[0];
    failed.status = ObservationStatus::kError;
    failed.native_source = "scripted-native";
    failed.native_code = 41;
    failed.retryable = true;
    result.items.push_back(std::move(failed));
    result.items.push_back(validItem(request.items[1], 22));
    return result;
  });
  CollectionCoordinator coordinator(environment.provider, environment.data,
                                    environment.clock);
  ASSERT_TRUE(coordinator.applyWatchSnapshot(environment.watchBoth()).ok());

  const CollectionRunResult run = coordinator.collectJob(
      1, MonotonicTime{Nanoseconds{100}}, MonotonicTime{Nanoseconds{300}});
  EXPECT_EQ(run.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  EXPECT_EQ(run.normalized_items, 2U);
  EXPECT_EQ(run.commit.committed_items, 2U);

  const DataReadResult read = environment.data.readLatest(
      {{environment.entity, MetricId{1}}, {environment.entity, MetricId{2}}},
      true, 150, environment.view->generation());
  ASSERT_EQ(read.items.size(), 2U);
  EXPECT_EQ(read.items[0].status, ObservationStatus::kError);
  EXPECT_EQ(read.items[0].error.native_code, 41);
  EXPECT_TRUE(read.items[0].error.retryable);
  EXPECT_EQ(read.items[1].status, ObservationStatus::kValid);
}

TEST(CollectionCoordinatorTest, ResultByteLimitRejectsBeforeCommit) {
  CollectionEnvironment environment([](const ProviderReadRequest &request) {
    ProviderReadResult result;
    result.call_status = Status::success();
    result.items.push_back(validItem(request.items.front(), 7));
    return result;
  });
  CollectionLimits limits;
  limits.max_result_bytes = 1;
  CollectionCoordinator coordinator(environment.provider, environment.data,
                                    environment.clock, limits);
  ASSERT_TRUE(coordinator.applyWatchSnapshot(environment.watchBoth()).ok());

  const CollectionRunResult run = coordinator.collectJob(
      1, MonotonicTime{Nanoseconds{100}}, MonotonicTime{Nanoseconds{300}});
  EXPECT_EQ(run.status.code(), PDCM_STATUS_RESOURCE_EXHAUSTED);
  EXPECT_EQ(environment.data.commitEpoch(), 0U);
}

TEST(CollectionCoordinatorTest, CallFailureCreatesExplainableItemFailures) {
  CollectionEnvironment environment([](const ProviderReadRequest &) {
    ProviderReadResult result;
    result.call_status = Status(PDCM_STATUS_UNAVAILABLE, "scripted outage");
    return result;
  });
  CollectionCoordinator coordinator(environment.provider, environment.data,
                                    environment.clock);
  ASSERT_TRUE(coordinator.applyWatchSnapshot(environment.watchBoth()).ok());

  const CollectionRunResult run = coordinator.collectJob(
      1, MonotonicTime{Nanoseconds{100}}, MonotonicTime{Nanoseconds{300}});
  EXPECT_EQ(run.status.code(), PDCM_STATUS_UNAVAILABLE);
  EXPECT_EQ(run.normalized_items, 2U);
  EXPECT_EQ(run.commit.status.code(), PDCM_STATUS_PARTIAL_RESULT);

  const DataReadResult read = environment.data.readLatest(
      {{environment.entity, MetricId{1}}, {environment.entity, MetricId{2}}},
      true, 150, environment.view->generation());
  ASSERT_EQ(read.items.size(), 2U);
  for (const Observation &item : read.items) {
    EXPECT_EQ(item.status, ObservationStatus::kNotAvailable);
    EXPECT_EQ(item.error.status.code(), PDCM_STATUS_UNAVAILABLE);
    EXPECT_FALSE(item.value.has_value());
  }
}

} // namespace
} // namespace pdcm
