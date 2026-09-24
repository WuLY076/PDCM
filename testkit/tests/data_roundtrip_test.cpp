#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "data/data_manager.hpp"
#include "data/query_engine.hpp"
#include "data/subscription_manager.hpp"
#include "pdcm/testkit/manual_clock.hpp"
#include "pdcm/testkit/mock_metric_ids.hpp"
#include "pdcm/testkit/mock_provider.hpp"
#include "semantic/semantic_catalog.hpp"

namespace pdcm::testkit {
namespace {

MetricDescriptor metricDescriptor(const std::uint32_t id,
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

TargetCatalog roundTripCatalog() {
  TargetCatalog catalog = TargetCatalog::blocked(TargetKind::kFpga);
  catalog.metrics_status = MetricsCatalogStatus::kReady;
  catalog.metrics = {
      metricDescriptor(kTestGaugeMetricId, "mock_gauge"),
      metricDescriptor(kTestCounterMetricId, "mock_counter"),
  };
  return catalog;
}

Observation observationFrom(const ProviderReadItemResult &item,
                            const std::uint64_t catalog_generation,
                            const ManualClock &clock) {
  Observation observation;
  observation.entity = item.item.entity;
  observation.metric = MetricId{item.item.data_id};
  observation.metric_semantic_version = 1;
  observation.value = item.value;
  observation.status = item.status;
  observation.source_sample_time_ns = item.source_sample_time_ns;
  observation.observed_monotonic_time_ns =
      clock.monotonicNow().time_since_epoch().count();
  observation.observed_wall_time_ns = clock.wallTimeNanoseconds();
  observation.catalog_generation = catalog_generation;
  observation.source.provider = "pdcm-mock-provider";
  observation.source.native_source = item.native_source;
  if (item.status != ObservationStatus::kValid) {
    observation.error.status =
        Status(PDCM_STATUS_UNAVAILABLE, "mock provider item failed");
    observation.error.native_code = item.native_code;
    observation.error.retryable = item.retryable;
  }
  return observation;
}

TEST(MockDataRoundTripTest, ProviderCommitQueryAndSubscriptionFormClosedLoop) {
  auto clock = std::make_shared<ManualClock>(100, 1000);
  MockProvider provider(MockProviderConfig{}, clock);
  ProviderInitOptions init;
  init.target = TargetKind::kFpga;
  init.deadline = clock->monotonicNow() + std::chrono::seconds(1);
  ASSERT_TRUE(provider.initialize(init).ok());

  const ProviderDiscoveryResult discovered =
      provider.discover(clock->monotonicNow() + std::chrono::seconds(1));
  ASSERT_TRUE(discovered.call_status.ok());

  const TargetCatalog target = roundTripCatalog();
  SemanticCatalog semantic(target);
  const CatalogCommitResult catalog_commit =
      semantic.commit(discovered.descriptor);
  ASSERT_TRUE(catalog_commit.status.ok());
  const std::shared_ptr<const CatalogView> catalog = semantic.snapshot();
  ASSERT_EQ(catalog->entities().size(), 1U);

  DataManager data;
  ASSERT_TRUE(data.activateCatalog(catalog, target).ok());
  SubscriptionManager subscriptions(data);
  SubscriptionFilter filter;
  filter.event_types = {EventType::kMetricUpdate};
  filter.entity = catalog->entities().front().ref;
  filter.metrics = {MetricId{kTestGaugeMetricId},
                    MetricId{kTestCounterMetricId}};
  const SubscribeResult subscribed = subscriptions.subscribe(filter);
  ASSERT_TRUE(subscribed.status.ok());

  ProviderReadRequest request;
  request.request_id = 1;
  request.plan_generation = 1;
  request.catalog_generation = catalog->generation();
  request.scheduled_time = clock->monotonicNow();
  request.deadline = clock->monotonicNow() + std::chrono::seconds(1);
  request.items = {
      ProviderReadItem{catalog->entities().front().ref,
                       ProviderDataKind::kMetric, kTestGaugeMetricId},
      ProviderReadItem{catalog->entities().front().ref,
                       ProviderDataKind::kMetric, kTestCounterMetricId},
  };
  const ProviderReadResult provider_result = provider.batchRead(request);
  ASSERT_TRUE(provider_result.call_status.ok());
  ASSERT_EQ(provider_result.items.size(), 2U);

  std::vector<Observation> observations;
  for (const ProviderReadItemResult &item : provider_result.items) {
    observations.push_back(
        observationFrom(item, catalog->generation(), *clock));
  }
  const DataCommitResult committed = data.commit(observations);
  ASSERT_TRUE(committed.status.ok());
  EXPECT_EQ(committed.committed_items, 2U);

  QueryEngine query(data, clock);
  QueryRequest query_request;
  query_request.selection = {
      DataKey{catalog->entities().front().ref, MetricId{kTestCounterMetricId}},
      DataKey{catalog->entities().front().ref, MetricId{kTestGaugeMetricId}},
  };
  const QueryResult queried = query.execute(query_request);
  ASSERT_TRUE(queried.status.ok());
  ASSERT_EQ(queried.items.size(), 2U);
  EXPECT_EQ(queried.items[0].metric.value, kTestGaugeMetricId);
  EXPECT_EQ(std::get<std::uint64_t>(*queried.items[0].value), 10U);
  EXPECT_EQ(queried.items[1].metric.value, kTestCounterMetricId);
  EXPECT_EQ(std::get<std::uint64_t>(*queried.items[1].value), 100U);

  ASSERT_TRUE(subscriptions.dispatch().ok());
  EXPECT_EQ(subscriptions.pendingCount(subscribed.subscription_id), 2U);
  std::uint64_t previous_sequence = 0;
  for (std::size_t index = 0; index < 2; ++index) {
    ASSERT_TRUE(subscriptions
                    .deliverNext(subscribed.subscription_id,
                                 [&previous_sequence](const PdcmEvent &event) {
                                   EXPECT_EQ(event.type,
                                             EventType::kMetricUpdate);
                                   EXPECT_GT(event.sequence, previous_sequence);
                                   previous_sequence = event.sequence;
                                 })
                    .ok());
  }
  EXPECT_EQ(subscriptions.pendingCount(subscribed.subscription_id), 0U);
}

} // namespace
} // namespace pdcm::testkit
