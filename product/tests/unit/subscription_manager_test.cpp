#include "data/subscription_manager.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace pdcm {
namespace {

TargetCatalog subscriptionCatalog() {
  TargetCatalog catalog = TargetCatalog::blocked(TargetKind::kFpga);
  catalog.metrics_status = MetricsCatalogStatus::kReady;
  catalog.metrics.clear();
  MetricDescriptor metric;
  metric.id = MetricId{1};
  metric.name = "subscription_counter";
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
  return catalog;
}

ProviderDescriptor subscriptionProvider() {
  ProviderDescriptor descriptor;
  descriptor.provider_version = "subscription-mock-provider";
  descriptor.target = TargetKind::kFpga;
  descriptor.state = ProviderState::kReady;
  descriptor.detected_device_count = 1;
  ProviderEntity entity;
  entity.stable_native_id = "device0";
  entity.pci_bdf = "0000:01:00.0";
  entity.pdrv_version = "subscription-pdrv";
  entity.incarnation = "boot0";
  entity.state = ProviderEntityState::kReady;
  entity.manageable = true;
  descriptor.entities.push_back(entity);
  return descriptor;
}

struct SubscriptionFixture {
  explicit SubscriptionFixture(DataStoreLimits limits = {})
      : semantic(subscriptionCatalog()), data(limits), subscriptions(data) {
    const CatalogCommitResult committed =
        semantic.commit(subscriptionProvider());
    if (!committed.status.ok()) {
      throw std::runtime_error("catalog commit failed");
    }
    catalog = semantic.snapshot();
    entity = catalog->entities().front().ref;
    const Status activated =
        data.activateCatalog(catalog, subscriptionCatalog());
    if (!activated.ok()) {
      throw std::runtime_error("catalog activation failed");
    }
  }

  DataCommitResult commit(const std::int64_t time_ns,
                          const std::uint64_t value) {
    Observation observation;
    observation.entity = entity;
    observation.metric = MetricId{1};
    observation.metric_semantic_version = 1;
    observation.value = value;
    observation.status = ObservationStatus::kValid;
    observation.observed_monotonic_time_ns = time_ns;
    observation.observed_wall_time_ns = time_ns;
    observation.catalog_generation = catalog->generation();
    observation.source.provider = "subscription.mock";
    return data.commit({observation});
  }

  SubscriptionFilter metricFilter(const bool follow = false) const {
    SubscriptionFilter filter;
    filter.event_types = {EventType::kMetricUpdate};
    filter.entity = entity;
    filter.metrics = {MetricId{1}};
    filter.follow_rediscovery = follow;
    return filter;
  }

  SemanticCatalog semantic;
  std::shared_ptr<const CatalogView> catalog;
  EntityRef entity;
  DataManager data;
  SubscriptionManager subscriptions;
};

TEST(SubscriptionManagerTest, CommitPublishesSequencedFilteredMetricEvent) {
  SubscriptionFixture fixture;
  const SubscribeResult subscribed =
      fixture.subscriptions.subscribe(fixture.metricFilter());
  ASSERT_TRUE(subscribed.status.ok());

  ASSERT_TRUE(fixture.commit(10, 42).status.ok());
  ASSERT_TRUE(fixture.subscriptions.dispatch().ok());
  ASSERT_EQ(fixture.subscriptions.pendingCount(subscribed.subscription_id), 1U);

  std::uint64_t delivered_sequence = 0;
  const Status delivered = fixture.subscriptions.deliverNext(
      subscribed.subscription_id,
      [&delivered_sequence](const PdcmEvent &event) {
        EXPECT_EQ(event.type, EventType::kMetricUpdate);
        EXPECT_EQ(event.metric, MetricId{1});
        const auto payload = std::get<MetricUpdatePayload>(event.payload);
        EXPECT_EQ(payload.status, ObservationStatus::kValid);
        EXPECT_EQ(payload.commit_epoch, 1U);
        delivered_sequence = event.sequence;
      });
  EXPECT_TRUE(delivered.ok());
  EXPECT_GT(delivered_sequence, 0U);
}

TEST(SubscriptionManagerTest, OverflowAddsOneLossMarkerAndIsolatesConsumer) {
  SubscriptionFixture fixture;
  const SubscribeResult slow = fixture.subscriptions.subscribe(
      fixture.metricFilter(), SubscriptionLimits{3, 4096});
  const SubscribeResult fast = fixture.subscriptions.subscribe(
      fixture.metricFilter(), SubscriptionLimits{8, 8192});
  ASSERT_TRUE(slow.status.ok());
  ASSERT_TRUE(fast.status.ok());

  std::size_t fast_deliveries = 0;
  for (std::uint64_t value = 1; value <= 5; ++value) {
    ASSERT_TRUE(fixture.commit(static_cast<std::int64_t>(value * 10), value)
                    .status.ok());
    ASSERT_TRUE(fixture.subscriptions.dispatch().ok());
    ASSERT_TRUE(fixture.subscriptions
                    .deliverNext(fast.subscription_id,
                                 [&fast_deliveries](const PdcmEvent &) {
                                   ++fast_deliveries;
                                 })
                    .ok());
  }

  EXPECT_EQ(fast_deliveries, 5U);
  EXPECT_LE(fixture.subscriptions.pendingCount(slow.subscription_id), 3U);
  EXPECT_LE(fixture.subscriptions.pendingBytes(slow.subscription_id), 4096U);

  std::size_t loss_markers = 0;
  std::uint64_t dropped_count = 0;
  while (fixture.subscriptions.pendingCount(slow.subscription_id) != 0) {
    ASSERT_TRUE(
        fixture.subscriptions
            .deliverNext(
                slow.subscription_id,
                [&loss_markers, &dropped_count](const PdcmEvent &event) {
                  if (event.type == EventType::kSubscriptionLossMarker) {
                    ++loss_markers;
                    dropped_count =
                        std::get<SubscriptionLossPayload>(event.payload)
                            .dropped_count;
                  }
                })
            .ok());
  }
  EXPECT_EQ(loss_markers, 1U);
  EXPECT_GT(dropped_count, 0U);
  EXPECT_FALSE(fixture.subscriptions.isClosed(fast.subscription_id));
}

TEST(SubscriptionManagerTest, NonDroppableOverflowClosesSubscription) {
  SubscriptionFixture fixture;
  SubscriptionFilter filter;
  filter.event_types = {EventType::kDaemonDraining};
  const SubscribeResult subscribed =
      fixture.subscriptions.subscribe(filter, SubscriptionLimits{1, 4096});
  ASSERT_TRUE(subscribed.status.ok());

  EventDraft draining;
  draining.type = EventType::kDaemonDraining;
  draining.severity = EventSeverity::kWarning;
  draining.catalog_generation = fixture.catalog->generation();
  ASSERT_TRUE(fixture.data.publishEvent(draining).status.ok());
  draining.occurrence_time_ns = 1;
  ASSERT_TRUE(fixture.data.publishEvent(draining).status.ok());
  ASSERT_TRUE(fixture.subscriptions.dispatch().ok());

  EXPECT_TRUE(fixture.subscriptions.isClosed(subscribed.subscription_id));
  EXPECT_EQ(fixture.subscriptions.pendingCount(subscribed.subscription_id), 0U);
}

TEST(SubscriptionManagerTest, CloseWaitsForActiveCallbackAndPreventsNewOnes) {
  SubscriptionFixture fixture;
  const SubscribeResult subscribed =
      fixture.subscriptions.subscribe(fixture.metricFilter());
  ASSERT_TRUE(subscribed.status.ok());
  ASSERT_TRUE(fixture.commit(10, 1).status.ok());
  ASSERT_TRUE(fixture.subscriptions.dispatch().ok());

  std::promise<void> callback_started;
  std::promise<void> release_callback;
  std::shared_future<void> release = release_callback.get_future().share();
  auto delivery = std::async(std::launch::async, [&] {
    return fixture.subscriptions.deliverNext(subscribed.subscription_id,
                                             [&](const PdcmEvent &) {
                                               callback_started.set_value();
                                               release.wait();
                                             });
  });
  callback_started.get_future().wait();

  auto closing = std::async(std::launch::async, [&] {
    return fixture.subscriptions.close(subscribed.subscription_id);
  });
  EXPECT_EQ(closing.wait_for(std::chrono::milliseconds(10)),
            std::future_status::timeout);
  release_callback.set_value();

  EXPECT_TRUE(delivery.get().ok());
  EXPECT_TRUE(closing.get().ok());
  EXPECT_EQ(
      fixture.subscriptions
          .deliverNext(subscribed.subscription_id, [](const PdcmEvent &) {})
          .code(),
      PDCM_STATUS_NOT_FOUND);
}

TEST(SubscriptionManagerTest, FollowRediscoveryRebindsLogicalEntity) {
  SubscriptionFixture fixture;
  const SubscribeResult exact =
      fixture.subscriptions.subscribe(fixture.metricFilter(false));
  const SubscribeResult following =
      fixture.subscriptions.subscribe(fixture.metricFilter(true));
  ASSERT_TRUE(exact.status.ok());
  ASSERT_TRUE(following.status.ok());

  EventDraft update;
  update.type = EventType::kMetricUpdate;
  update.entity = fixture.entity;
  update.entity->generation += 1;
  update.metric = MetricId{1};
  update.catalog_generation = fixture.catalog->generation();
  update.payload = MetricUpdatePayload{ObservationStatus::kValid, 1};
  ASSERT_TRUE(fixture.data.publishEvent(update).status.ok());
  ASSERT_TRUE(fixture.subscriptions.dispatch().ok());

  EXPECT_EQ(fixture.subscriptions.pendingCount(exact.subscription_id), 0U);
  EXPECT_EQ(fixture.subscriptions.pendingCount(following.subscription_id), 1U);
}

TEST(SubscriptionManagerTest,
     DrainPublishesFinalEventAndRejectsNewSubscriptions) {
  SubscriptionFixture fixture;
  SubscriptionFilter filter;
  filter.event_types = {EventType::kDaemonDraining};
  const SubscribeResult subscribed = fixture.subscriptions.subscribe(filter);
  ASSERT_TRUE(subscribed.status.ok());

  EXPECT_TRUE(fixture.subscriptions.drain(100).ok());
  EXPECT_EQ(fixture.subscriptions.pendingCount(subscribed.subscription_id), 1U);
  EXPECT_EQ(fixture.subscriptions.subscribe(filter).status.code(),
            PDCM_STATUS_UNAVAILABLE);
}

TEST(EventStoreTest, DataManagerMaintainsHardEventItemAndByteLimits) {
  DataStoreLimits limits;
  limits.max_events = 2;
  limits.max_event_bytes = 1024;
  SubscriptionFixture fixture(limits);

  ASSERT_TRUE(fixture.commit(10, 1).status.ok());
  ASSERT_TRUE(fixture.commit(20, 2).status.ok());
  ASSERT_TRUE(fixture.commit(30, 3).status.ok());

  EXPECT_LE(fixture.data.eventCount(), 2U);
  EXPECT_LE(fixture.data.eventBytes(), limits.max_event_bytes);
  const EventSnapshot snapshot = fixture.data.eventsSince(0);
  ASSERT_EQ(snapshot.events.size(), 2U);
  EXPECT_LT(snapshot.events[0]->sequence, snapshot.events[1]->sequence);
}

} // namespace
} // namespace pdcm
