#include "data/query_engine.hpp"

#include <cstdint>
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
  explicit TestClock(const std::int64_t now_ns) : now_ns_(now_ns) {}

  void set(const std::int64_t now_ns) { now_ns_ = now_ns; }

  [[nodiscard]] MonotonicTime monotonicNow() const noexcept override {
    return MonotonicTime{Nanoseconds{now_ns_}};
  }

  [[nodiscard]] std::int64_t wallTimeNanoseconds() const noexcept override {
    return now_ns_;
  }

private:
  std::int64_t now_ns_;
};

TargetCatalog queryCatalog() {
  TargetCatalog catalog = TargetCatalog::blocked(TargetKind::kFpga);
  catalog.metrics_status = MetricsCatalogStatus::kReady;
  catalog.metrics.clear();
  for (std::uint32_t id = 1; id <= 2; ++id) {
    MetricDescriptor metric;
    metric.id = MetricId{id};
    metric.name = "metric_" + std::to_string(id);
    metric.value_type = MetricValueKind::kUint64;
    metric.unit = "count";
    metric.scope = EntityKind::kDevice;
    metric.default_period_ns = 10;
    metric.min_period_ns = 1;
    metric.freshness_ns = 20;
    metric.supports_fpga = true;
    metric.requirement = RequirementLevel::kRequired;
    metric.semantic_version = 1;
    metric.provider_mapping_approved = true;
    catalog.metrics.push_back(metric);
  }
  return catalog;
}

ProviderDescriptor queryProvider() {
  ProviderDescriptor descriptor;
  descriptor.provider_version = "query-mock-provider";
  descriptor.target = TargetKind::kFpga;
  descriptor.state = ProviderState::kReady;
  descriptor.detected_device_count = 1;
  ProviderEntity entity;
  entity.stable_native_id = "device0";
  entity.pci_bdf = "0000:01:00.0";
  entity.pdrv_version = "query-pdrv";
  entity.incarnation = "boot0";
  entity.state = ProviderEntityState::kReady;
  entity.manageable = true;
  descriptor.entities.push_back(entity);
  return descriptor;
}

Observation sample(const DataKey &key, const std::uint64_t generation,
                   const std::int64_t time_ns, const std::uint64_t value) {
  Observation observation;
  observation.entity = key.entity;
  observation.metric = key.metric;
  observation.metric_semantic_version = 1;
  observation.value = value;
  observation.status = ObservationStatus::kValid;
  observation.observed_monotonic_time_ns = time_ns;
  observation.observed_wall_time_ns = time_ns;
  observation.catalog_generation = generation;
  observation.source.provider = "query.mock";
  return observation;
}

class FakeFreshRead final : public FreshReadCoordinator {
public:
  FakeFreshRead(DataManager &data_manager, std::shared_ptr<TestClock> clock)
      : data_manager_(data_manager), clock_(std::move(clock)) {}

  [[nodiscard]] Status freshRead(const std::vector<DataKey> &keys,
                                 const std::uint64_t catalog_generation,
                                 const MonotonicTime deadline) override {
    (void)deadline;
    ++calls;
    last_keys = keys;
    std::vector<Observation> observations;
    observations.reserve(keys.size());
    for (const DataKey &key : keys) {
      Observation observation =
          sample(key, catalog_generation,
                 clock_->monotonicNow().time_since_epoch().count(), next_value);
      if (fail) {
        observation.value.reset();
        observation.status = ObservationStatus::kError;
        observation.error.status =
            Status(PDCM_STATUS_UNAVAILABLE, "fresh read failed");
        observation.error.retryable = true;
      }
      observations.push_back(std::move(observation));
    }
    const DataCommitResult committed = data_manager_.commit(observations);
    if (fail) {
      EXPECT_EQ(committed.status.code(), PDCM_STATUS_PARTIAL_RESULT);
      return Status(PDCM_STATUS_UNAVAILABLE, "fresh read failed");
    }
    EXPECT_TRUE(committed.status.ok());
    return committed.status;
  }

  bool fail{false};
  std::uint64_t next_value{99};
  std::size_t calls{0};
  std::vector<DataKey> last_keys;

private:
  DataManager &data_manager_;
  std::shared_ptr<TestClock> clock_;
};

struct QueryFixture {
  QueryFixture() : semantic(queryCatalog()) {
    const CatalogCommitResult committed = semantic.commit(queryProvider());
    if (!committed.status.ok()) {
      throw std::runtime_error("catalog commit failed");
    }
    catalog = semantic.snapshot();
    entity = catalog->entities().front().ref;
    const Status activated = data.activateCatalog(catalog, queryCatalog());
    if (!activated.ok()) {
      throw std::runtime_error("catalog activation failed");
    }
  }

  DataKey key(const std::uint32_t metric) const {
    return DataKey{entity, MetricId{metric}};
  }

  SemanticCatalog semantic;
  std::shared_ptr<const CatalogView> catalog;
  EntityRef entity;
  DataManager data;
};

TEST(QueryEngineTest, CacheOnlyReturnsDeterministicOrderWithoutFreshRead) {
  QueryFixture fixture;
  auto clock = std::make_shared<TestClock>(10);
  FakeFreshRead fresh(fixture.data, clock);
  ASSERT_TRUE(
      fixture.data
          .commit(
              {sample(fixture.key(2), fixture.catalog->generation(), 10, 2),
               sample(fixture.key(1), fixture.catalog->generation(), 10, 1)})
          .status.ok());
  QueryEngine engine(fixture.data, clock, &fresh);

  QueryRequest request;
  request.selection = {fixture.key(2), fixture.key(1)};
  const QueryResult result = engine.execute(request);

  ASSERT_TRUE(result.status.ok());
  ASSERT_EQ(result.items.size(), 2U);
  EXPECT_EQ(result.items[0].metric.value, 1U);
  EXPECT_EQ(result.items[1].metric.value, 2U);
  EXPECT_EQ(fresh.calls, 0U);
}

TEST(QueryEngineTest, CacheOrFreshCoalescesExpiredKeysIntoOneRead) {
  QueryFixture fixture;
  auto clock = std::make_shared<TestClock>(100);
  FakeFreshRead fresh(fixture.data, clock);
  ASSERT_TRUE(
      fixture.data
          .commit(
              {sample(fixture.key(1), fixture.catalog->generation(), 10, 1),
               sample(fixture.key(2), fixture.catalog->generation(), 10, 2)})
          .status.ok());
  QueryEngine engine(fixture.data, clock, &fresh);

  QueryRequest request;
  request.selection = {fixture.key(2), fixture.key(1)};
  request.read_policy = ReadPolicy::kCacheOrFresh;
  request.max_age_ns = 50;
  const QueryResult result = engine.execute(request);

  ASSERT_TRUE(result.status.ok());
  ASSERT_EQ(result.items.size(), 2U);
  EXPECT_EQ(fresh.calls, 1U);
  EXPECT_EQ(fresh.last_keys.size(), 2U);
  EXPECT_EQ(std::get<std::uint64_t>(*result.items[0].value), 99U);
}

TEST(QueryEngineTest, FreshFailureReturnsStaleValueAndLatestFailure) {
  QueryFixture fixture;
  auto clock = std::make_shared<TestClock>(100);
  FakeFreshRead fresh(fixture.data, clock);
  fresh.fail = true;
  ASSERT_TRUE(fixture.data
                  .commit({sample(fixture.key(1), fixture.catalog->generation(),
                                  10, 42)})
                  .status.ok());
  QueryEngine engine(fixture.data, clock, &fresh);

  QueryRequest request;
  request.selection = {fixture.key(1)};
  request.read_policy = ReadPolicy::kFreshRequired;
  request.allow_stale = true;
  const QueryResult result = engine.execute(request);

  EXPECT_EQ(result.status.code(), PDCM_STATUS_UNAVAILABLE);
  ASSERT_EQ(result.items.size(), 1U);
  EXPECT_EQ(result.items.front().status, ObservationStatus::kStale);
  EXPECT_EQ(result.items.front().stale_age_ns, 90);
  ASSERT_TRUE(result.items.front().latest_failure.has_value());
  EXPECT_EQ(result.items.front().latest_failure->status.code(),
            PDCM_STATUS_UNAVAILABLE);
}

TEST(QueryEngineTest, FreshReadHonorsExpiredDeadline) {
  QueryFixture fixture;
  auto clock = std::make_shared<TestClock>(100);
  FakeFreshRead fresh(fixture.data, clock);
  ASSERT_TRUE(fixture.data
                  .commit({sample(fixture.key(1), fixture.catalog->generation(),
                                  10, 42)})
                  .status.ok());
  QueryEngine engine(fixture.data, clock, &fresh);

  QueryRequest request;
  request.selection = {fixture.key(1)};
  request.read_policy = ReadPolicy::kFreshRequired;
  request.deadline = MonotonicTime{Nanoseconds{99}};
  const QueryResult result = engine.execute(request);

  EXPECT_EQ(result.status.code(), PDCM_STATUS_TIMEOUT);
  EXPECT_EQ(fresh.calls, 0U);
}

TEST(QueryEngineTest, ReportsPartialResultForUnavailableAndUnsupportedItems) {
  QueryFixture fixture;
  auto clock = std::make_shared<TestClock>(10);
  QueryEngine engine(fixture.data, clock);

  QueryRequest request;
  request.selection = {fixture.key(99), fixture.key(1)};
  const QueryResult result = engine.execute(request);

  EXPECT_EQ(result.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  ASSERT_EQ(result.items.size(), 2U);
  EXPECT_EQ(result.items[0].status, ObservationStatus::kNotAvailable);
  EXPECT_EQ(result.items[1].status, ObservationStatus::kUnsupported);
}

TEST(QueryEngineTest, TruncatesToDeterministicPrefixAndReportsRequiredSize) {
  QueryFixture fixture;
  auto clock = std::make_shared<TestClock>(10);
  ASSERT_TRUE(
      fixture.data
          .commit(
              {sample(fixture.key(1), fixture.catalog->generation(), 10, 1),
               sample(fixture.key(2), fixture.catalog->generation(), 10, 2)})
          .status.ok());
  QueryEngine engine(fixture.data, clock);

  QueryRequest request;
  request.selection = {fixture.key(2), fixture.key(1)};
  request.max_result_items = 1;
  const QueryResult result = engine.execute(request);

  EXPECT_EQ(result.status.code(), PDCM_STATUS_BUFFER_TOO_SMALL);
  EXPECT_TRUE(result.truncated);
  EXPECT_EQ(result.required_items, 2U);
  ASSERT_EQ(result.items.size(), 1U);
  EXPECT_EQ(result.items.front().metric.value, 1U);
}

TEST(QueryEngineTest, SameBatchRejectsMixedCommitEpochs) {
  QueryFixture fixture;
  auto clock = std::make_shared<TestClock>(10);
  ASSERT_TRUE(
      fixture.data
          .commit({sample(fixture.key(1), fixture.catalog->generation(), 9, 1)})
          .status.ok());
  ASSERT_TRUE(fixture.data
                  .commit({sample(fixture.key(2), fixture.catalog->generation(),
                                  10, 2)})
                  .status.ok());
  QueryEngine engine(fixture.data, clock);

  QueryRequest request;
  request.selection = {fixture.key(1), fixture.key(2)};
  request.consistency = QueryConsistency::kSameBatch;
  const QueryResult result = engine.execute(request);

  EXPECT_EQ(result.status.code(), PDCM_STATUS_UNAVAILABLE);
  EXPECT_TRUE(result.items.empty());
}

TEST(QueryEngineTest, AtLeastEpochHonorsDeadlineAndItemEpoch) {
  QueryFixture fixture;
  auto clock = std::make_shared<TestClock>(10);
  ASSERT_TRUE(fixture.data
                  .commit({sample(fixture.key(1), fixture.catalog->generation(),
                                  10, 1)})
                  .status.ok());
  QueryEngine engine(fixture.data, clock);

  QueryRequest request;
  request.selection = {fixture.key(1)};
  request.consistency = QueryConsistency::kAtLeastEpoch;
  request.required_epoch = 2;
  request.deadline = MonotonicTime{Nanoseconds{0}};
  const QueryResult result = engine.execute(request);

  EXPECT_EQ(result.status.code(), PDCM_STATUS_TIMEOUT);
  EXPECT_TRUE(result.items.empty());
}

} // namespace
} // namespace pdcm
