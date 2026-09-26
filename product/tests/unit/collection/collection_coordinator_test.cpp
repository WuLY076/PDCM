#include "collection/collection_coordinator.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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
    return MonotonicTime{Nanoseconds{monotonic_ns_.load()}};
  }

  [[nodiscard]] std::int64_t wallTimeNanoseconds() const noexcept override {
    return wall_ns_.load();
  }

  void advance(const Nanoseconds delta) noexcept {
    monotonic_ns_.fetch_add(delta.count());
    wall_ns_.fetch_add(delta.count());
  }

  void set(const std::int64_t monotonic_ns,
           const std::int64_t wall_ns) noexcept {
    monotonic_ns_.store(monotonic_ns);
    wall_ns_.store(wall_ns);
  }

private:
  std::atomic<std::int64_t> monotonic_ns_;
  std::atomic<std::int64_t> wall_ns_;
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

struct BlockingRead {
  ProviderReadResult read(const ProviderReadRequest &request) {
    {
      std::unique_lock<std::mutex> lock(mutex);
      ++calls;
      entered = true;
      changed.notify_all();
      changed.wait(lock, [this] { return released; });
    }

    ProviderReadResult result;
    result.call_status = Status::success();
    for (const ProviderReadItem &item : request.items) {
      result.items.push_back(validItem(item, item.data_id * 10U));
    }
    return result;
  }

  bool waitUntilEntered() {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(1),
                            [this] { return entered; });
  }

  void unblock() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      released = true;
    }
    changed.notify_all();
  }

  std::size_t callCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return calls;
  }

  mutable std::mutex mutex;
  std::condition_variable changed;
  bool entered{false};
  bool released{false};
  std::size_t calls{0};
};
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

TEST(CollectionCoordinatorTest, SchedulerHasNoDriftBurstOrConcurrentSameKey) {
  const auto blocker = std::make_shared<BlockingRead>();
  CollectionEnvironment environment(
      [blocker](const ProviderReadRequest &request) {
        return blocker->read(request);
      });
  CollectionCoordinator coordinator(environment.provider, environment.data,
                                    environment.clock);
  ASSERT_TRUE(coordinator.applyWatchSnapshot(environment.watchBoth()).ok());

  environment.clock->advance(Nanoseconds{100});
  SchedulerRunSummary first =
      coordinator.runDueOnce(environment.clock->monotonicNow());
  EXPECT_EQ(first.enqueued_jobs, 1U);
  ASSERT_TRUE(blocker->waitUntilEntered());

  environment.clock->advance(Nanoseconds{100});
  SchedulerRunSummary busy =
      coordinator.runDueOnce(environment.clock->monotonicNow());
  EXPECT_EQ(busy.busy_jobs, 1U);

  environment.clock->advance(Nanoseconds{200});
  SchedulerRunSummary missed =
      coordinator.runDueOnce(environment.clock->monotonicNow());
  EXPECT_EQ(missed.missed_periods, 1U);
  EXPECT_EQ(missed.busy_jobs, 1U);
  EXPECT_EQ(blocker->callCount(), 1U);

  blocker->unblock();
  const MonotonicTime real_deadline = std::chrono::time_point_cast<Nanoseconds>(
                                          std::chrono::steady_clock::now()) +
                                      std::chrono::seconds(1);
  ASSERT_TRUE(environment.data.waitForEpoch(1, real_deadline).ok());

  SchedulerRunSummary resumed;
  MonotonicTime resumed_time;
  for (std::size_t attempt = 0; attempt < 1000U && resumed.enqueued_jobs == 0;
       ++attempt) {
    std::this_thread::yield();
    environment.clock->advance(Nanoseconds{100});
    resumed_time = environment.clock->monotonicNow();
    resumed = coordinator.runDueOnce(resumed_time);
  }
  ASSERT_EQ(resumed.enqueued_jobs, 1U);
  const MonotonicTime second_deadline =
      std::chrono::time_point_cast<Nanoseconds>(
          std::chrono::steady_clock::now()) +
      std::chrono::seconds(1);
  ASSERT_TRUE(environment.data.waitForEpoch(2, second_deadline).ok());
  ASSERT_EQ(environment.scripted->requests.size(), 2U);
  EXPECT_EQ(environment.scripted->requests[0].scheduled_time,
            MonotonicTime{Nanoseconds{250}});
  EXPECT_EQ(environment.scripted->requests[1].scheduled_time, resumed_time);
}

TEST(CollectionCoordinatorTest, SchedulerQueueIsBounded) {
  CollectionEnvironment environment([](const ProviderReadRequest &request) {
    ProviderReadResult result;
    result.call_status = Status::success();
    for (const ProviderReadItem &item : request.items) {
      result.items.push_back(validItem(item, item.data_id));
    }
    return result;
  });
  CollectionLimits limits;
  limits.max_batch_items = 1;
  limits.max_provider_queue = 1;
  CollectionCoordinator coordinator(environment.provider, environment.data,
                                    environment.clock, limits);
  ASSERT_TRUE(coordinator.applyWatchSnapshot(environment.watchBoth()).ok());

  environment.clock->advance(Nanoseconds{100});
  const SchedulerRunSummary run =
      coordinator.runDueOnce(environment.clock->monotonicNow());
  EXPECT_EQ(run.enqueued_jobs, 1U);
  EXPECT_EQ(run.queue_rejections, 1U);
  EXPECT_EQ(run.status.code(), PDCM_STATUS_PARTIAL_RESULT);
}

TEST(CollectionCoordinatorTest, FreshReadCoalescesAndIsolatesWaiterTimeout) {
  const auto blocker = std::make_shared<BlockingRead>();
  CollectionEnvironment environment(
      [blocker](const ProviderReadRequest &request) {
        return blocker->read(request);
      });
  const MonotonicTime system_now = std::chrono::time_point_cast<Nanoseconds>(
      std::chrono::steady_clock::now());
  environment.clock->set(system_now.time_since_epoch().count(), 10000);

  CollectionCoordinator coordinator(environment.provider, environment.data,
                                    environment.clock);
  ASSERT_TRUE(coordinator.activateCatalog(environment.view).ok());
  ASSERT_TRUE(coordinator.applyWatchSnapshot(environment.watchBoth()).ok());
  const std::vector<DataKey> keys = {
      {environment.entity, MetricId{1}},
      {environment.entity, MetricId{2}},
  };

  Status long_waiter;
  const MonotonicTime long_deadline = system_now + std::chrono::seconds(2);
  std::thread first([&] {
    long_waiter = coordinator.freshRead(keys, environment.view->generation(),
                                        long_deadline);
  });
  ASSERT_TRUE(blocker->waitUntilEntered());

  const MonotonicTime short_deadline =
      std::chrono::time_point_cast<Nanoseconds>(
          std::chrono::steady_clock::now()) +
      std::chrono::milliseconds(50);
  const Status short_waiter = coordinator.freshRead(
      keys, environment.view->generation(), short_deadline);
  EXPECT_EQ(short_waiter.code(), PDCM_STATUS_TIMEOUT);
  EXPECT_EQ(blocker->callCount(), 1U);

  blocker->unblock();
  first.join();
  EXPECT_TRUE(long_waiter.ok());
  EXPECT_EQ(blocker->callCount(), 1U);
  EXPECT_EQ(environment.data.commitEpoch(), 1U);
}
TEST(CollectionCoordinatorTest, FreshReadNeedsNoPermanentWatch) {
  CollectionEnvironment environment([](const ProviderReadRequest &request) {
    ProviderReadResult result;
    result.call_status = Status::success();
    for (const ProviderReadItem &item : request.items) {
      result.items.push_back(validItem(item, 33));
    }
    return result;
  });
  const MonotonicTime system_now = std::chrono::time_point_cast<Nanoseconds>(
      std::chrono::steady_clock::now());
  environment.clock->set(system_now.time_since_epoch().count(), 10000);

  CollectionCoordinator coordinator(environment.provider, environment.data,
                                    environment.clock);
  ASSERT_TRUE(coordinator.activateCatalog(environment.view).ok());
  EXPECT_TRUE(coordinator.plan()->jobs.empty());
  ASSERT_TRUE(coordinator
                  .freshRead({{environment.entity, MetricId{1}}},
                             environment.view->generation(),
                             system_now + std::chrono::seconds(1))
                  .ok());
  EXPECT_TRUE(coordinator.plan()->jobs.empty());
  EXPECT_EQ(environment.data.commitEpoch(), 1U);

  EXPECT_FALSE(coordinator.schedulerRunning());
  ASSERT_TRUE(coordinator.startScheduler().ok());
  EXPECT_TRUE(coordinator.schedulerRunning());
  coordinator.stopScheduler();
  EXPECT_FALSE(coordinator.schedulerRunning());
}
TEST(CollectionCoordinatorTest, InFlightOldGenerationCannotCommit) {
  const auto blocker = std::make_shared<BlockingRead>();
  CollectionEnvironment environment(
      [blocker](const ProviderReadRequest &request) {
        return blocker->read(request);
      });
  CollectionCoordinator coordinator(environment.provider, environment.data,
                                    environment.clock);
  ASSERT_TRUE(coordinator.applyWatchSnapshot(environment.watchBoth()).ok());

  CollectionRunResult collected;
  std::thread read([&] {
    collected = coordinator.collectJob(1, MonotonicTime{Nanoseconds{100}},
                                       MonotonicTime{Nanoseconds{300}});
  });
  ASSERT_TRUE(blocker->waitUntilEntered());

  const CatalogCommitResult changed =
      environment.semantic.commit(collectionProvider());
  ASSERT_TRUE(changed.status.ok());
  environment.view = environment.semantic.snapshot();
  ASSERT_TRUE(
      environment.data.activateCatalog(environment.view, environment.target)
          .ok());

  blocker->unblock();
  read.join();
  EXPECT_EQ(collected.status.code(), PDCM_STATUS_STALE_GENERATION);
  EXPECT_EQ(environment.data.commitEpoch(), 0U);
}
TEST(CollectionCoordinatorTest, SampleLimitCountsFailuresAndRecompilesPlan) {
  CollectionEnvironment environment([](const ProviderReadRequest &) {
    ProviderReadResult result;
    result.call_status =
        Status(PDCM_STATUS_UNAVAILABLE, "scripted provider unavailable");
    return result;
  });

  WatchRequirement requirement;
  requirement.catalog_generation = environment.view->generation();
  requirement.entity = environment.entity;
  requirement.metrics = {MetricId{1}};
  requirement.period = Nanoseconds{100};
  requirement.freshness = Nanoseconds{200};
  requirement.retention = Nanoseconds{400};
  requirement.sample_limit = 2;
  ASSERT_TRUE(environment.watches
                  .create(WatchOwner{WatchOwnerKind::kSession, 7},
                          std::move(requirement))
                  .status.ok());

  CollectionCoordinator coordinator(environment.provider, environment.data,
                                    environment.clock, {},
                                    &environment.watches);
  ASSERT_TRUE(
      coordinator.applyWatchSnapshot(environment.watches.snapshot()).ok());
  ASSERT_EQ(coordinator.plan()->jobs.size(), 1U);

  (void)coordinator.collectJob(1, MonotonicTime{Nanoseconds{100}},
                               MonotonicTime{Nanoseconds{300}});
  EXPECT_EQ(environment.watches.snapshot()->logical_watches.size(), 1U);
  EXPECT_EQ(coordinator.plan()->jobs.size(), 1U);

  (void)coordinator.collectJob(1, MonotonicTime{Nanoseconds{200}},
                               MonotonicTime{Nanoseconds{400}});
  EXPECT_TRUE(environment.watches.snapshot()->logical_watches.empty());
  EXPECT_TRUE(coordinator.plan()->jobs.empty());
}

} // namespace
} // namespace pdcm
