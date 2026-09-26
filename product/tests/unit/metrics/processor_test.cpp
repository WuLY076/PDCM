#include "metrics/counter_processor.hpp"
#include "metrics/processor.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "semantic/semantic_catalog.hpp"

namespace pdcm {
namespace {

MetricDescriptor
metricDescriptor(const std::uint32_t id, std::string name,
                 const MetricCollectionMode mode,
                 const MetricValueKind value_type = MetricValueKind::kUint64,
                 std::vector<MetricId> dependencies = {}) {
  MetricDescriptor descriptor;
  descriptor.id = MetricId{id};
  descriptor.name = std::move(name);
  descriptor.value_type = value_type;
  descriptor.unit =
      value_type == MetricValueKind::kDouble ? "per_second" : "count";
  descriptor.temporality = mode == MetricCollectionMode::kDerived
                               ? MetricTemporality::kGauge
                               : MetricTemporality::kCumulativeCounter;
  descriptor.collection_mode = mode;
  descriptor.default_period_ns = 10;
  descriptor.min_period_ns = 1;
  descriptor.freshness_ns = 100;
  descriptor.supports_fpga = true;
  descriptor.requirement = RequirementLevel::kConditional;
  descriptor.semantic_version = 1;
  descriptor.provider_mapping_approved = mode != MetricCollectionMode::kDerived;
  descriptor.dependencies = std::move(dependencies);
  return descriptor;
}

TargetCatalog counterCatalog() {
  TargetCatalog catalog = TargetCatalog::blocked(TargetKind::kFpga);
  catalog.metrics_status = MetricsCatalogStatus::kReady;
  catalog.metrics.push_back(
      metricDescriptor(1, "test_raw_counter", MetricCollectionMode::kPoll));
  catalog.metrics.push_back(
      metricDescriptor(2, "test_counter_delta", MetricCollectionMode::kDerived,
                       MetricValueKind::kUint64, {MetricId{1}}));
  catalog.metrics.push_back(
      metricDescriptor(3, "test_counter_rate", MetricCollectionMode::kDerived,
                       MetricValueKind::kDouble, {MetricId{1}}));
  return catalog;
}

CounterProcessorConfig counterConfig() {
  CounterProcessorConfig config;
  config.processor_id = "test.counter";
  config.processor_version = 1;
  config.input = ProcessorMetric{MetricId{1}, 1};
  config.delta_output = ProcessorMetric{MetricId{2}, 1};
  config.rate_output = ProcessorMetric{MetricId{3}, 1};
  config.minimum_interval_ns = 1;
  config.maximum_gap_ns = 100;
  return config;
}

EntityRef testEntity() {
  return EntityRef{EntityKind::kDevice, EntityId{7}, 1};
}

Observation counterSample(const std::uint64_t value, const std::int64_t time,
                          const std::uint64_t commit_epoch,
                          const std::uint64_t counter_epoch = 1) {
  Observation observation;
  observation.entity = testEntity();
  observation.metric = MetricId{1};
  observation.value = value;
  observation.status = ObservationStatus::kValid;
  observation.scheduled_monotonic_time_ns = time;
  observation.source_sample_time_ns = time;
  observation.observed_monotonic_time_ns = time;
  observation.metric_semantic_version = 1;
  observation.observed_wall_time_ns = time + 1000;
  observation.catalog_generation = 1;
  observation.commit_epoch = commit_epoch;
  observation.counter_epoch = counter_epoch;
  observation.source.provider = "test-provider";
  observation.source.native_source = "test-counter";
  return observation;
}

ProcessorContext processorContext(const std::int64_t evaluated_time = 20) {
  return ProcessorContext{testEntity(), 1, evaluated_time, 1};
}

std::shared_ptr<const CounterDeltaRateProcessor> counterProcessor() {
  return std::make_shared<const CounterDeltaRateProcessor>(counterConfig());
}

ProviderDescriptor providerDescriptor(std::string pdrv = "pdrv-1") {
  ProviderDescriptor descriptor;
  descriptor.provider_version = "processor-test-provider";
  descriptor.target = TargetKind::kFpga;
  descriptor.state = ProviderState::kReady;
  descriptor.detected_device_count = 1;
  ProviderEntity entity;
  entity.stable_native_id = "processor-device";
  entity.pci_bdf = "0000:01:00.0";
  entity.pdrv_version = std::move(pdrv);
  entity.incarnation = "boot-1";
  entity.state = ProviderEntityState::kReady;
  entity.manageable = true;
  descriptor.entities.push_back(std::move(entity));
  descriptor.capabilities.push_back(
      ProviderCapability{ProviderDataKind::kMetric, 1, true, "TEST_SUPPORTED"});
  return descriptor;
}

CounterProcessorConfig
singleOutputConfig(std::string id, const std::uint32_t input,
                   const std::uint32_t output,
                   const std::uint32_t input_semantic_version = 1) {
  CounterProcessorConfig config;
  config.processor_id = std::move(id);
  config.processor_version = 1;
  config.input = ProcessorMetric{MetricId{input}, input_semantic_version};
  config.delta_output = ProcessorMetric{MetricId{output}, 1};
  config.minimum_interval_ns = 1;
  config.maximum_gap_ns = 100;
  return config;
}

TEST(ProcessorGraphTest, BuildsDeterministicGraphAndKeepsBlockedCatalogEmpty) {
  ProcessorGraph graph;
  TargetCatalog catalog = counterCatalog();
  const Status built = graph.rebuild(catalog, 1, {counterProcessor()});
  ASSERT_TRUE(built.ok());
  const std::shared_ptr<const ProcessorGraphSnapshot> snapshot =
      graph.snapshot();
  ASSERT_EQ(snapshot->nodes.size(), 1);
  EXPECT_EQ(snapshot->nodes.front().processor->id(), "test.counter");
  EXPECT_EQ(snapshot->nodes.front().depth, 1);

  TargetCatalog blocked = TargetCatalog::blocked(TargetKind::kFpga);
  ASSERT_TRUE(graph.rebuild(blocked, 2, {}).ok());
  EXPECT_TRUE(graph.snapshot()->nodes.empty());
}

TEST(ProcessorGraphTest, RejectsMissingOutputSemanticMismatchAndCycle) {
  ProcessorGraph graph;
  TargetCatalog missing = counterCatalog();
  EXPECT_EQ(graph.rebuild(missing, 1, {}).code(), PDCM_STATUS_UNSUPPORTED);

  CounterProcessorConfig mismatch = counterConfig();
  mismatch.input.semantic_version = 2;
  EXPECT_EQ(graph
                .rebuild(counterCatalog(), 1,
                         {std::make_shared<const CounterDeltaRateProcessor>(
                             mismatch)})
                .code(),
            PDCM_STATUS_UNSUPPORTED);

  TargetCatalog cycle = TargetCatalog::blocked(TargetKind::kFpga);
  cycle.metrics_status = MetricsCatalogStatus::kReady;
  cycle.metrics.push_back(
      metricDescriptor(10, "test_cycle_a", MetricCollectionMode::kDerived,
                       MetricValueKind::kUint64, {MetricId{11}}));
  cycle.metrics.push_back(
      metricDescriptor(11, "test_cycle_b", MetricCollectionMode::kDerived,
                       MetricValueKind::kUint64, {MetricId{10}}));
  std::vector<std::shared_ptr<const MetricProcessor>> processors;
  processors.push_back(std::make_shared<const CounterDeltaRateProcessor>(
      singleOutputConfig("cycle.a", 11, 10)));
  processors.push_back(std::make_shared<const CounterDeltaRateProcessor>(
      singleOutputConfig("cycle.b", 10, 11)));
  EXPECT_EQ(graph.rebuild(cycle, 1, processors).code(),
            PDCM_STATUS_UNSUPPORTED);
}

TEST(ProcessorGraphTest, RejectsDerivationDepthAboveFour) {
  TargetCatalog catalog = TargetCatalog::blocked(TargetKind::kFpga);
  catalog.metrics_status = MetricsCatalogStatus::kReady;
  catalog.metrics.push_back(
      metricDescriptor(1, "test_depth_raw", MetricCollectionMode::kPoll));
  std::vector<std::shared_ptr<const MetricProcessor>> processors;
  for (std::uint32_t index = 1; index <= 5; ++index) {
    const std::uint32_t output = index + 1;
    catalog.metrics.push_back(
        metricDescriptor(output, "test_depth_" + std::to_string(index),
                         MetricCollectionMode::kDerived,
                         MetricValueKind::kUint64, {MetricId{index}}));
    processors.push_back(std::make_shared<const CounterDeltaRateProcessor>(
        singleOutputConfig("depth." + std::to_string(index), index, output)));
  }

  ProcessorGraph graph;
  EXPECT_EQ(graph.rebuild(catalog, 1, processors).code(),
            PDCM_STATUS_UNSUPPORTED);
}

TEST(CounterProcessorTest, ComputesNormalDeltaAndRateWithProvenance) {
  const CounterDeltaRateProcessor processor(counterConfig());
  const ProcessorEvaluation evaluated = processor.evaluate(
      processorContext(20), ObservationSnapshot{{counterSample(10, 10, 1),
                                                 counterSample(25, 20, 2)}});

  ASSERT_TRUE(evaluated.status.ok());
  ASSERT_EQ(evaluated.outputs.size(), 2);
  ASSERT_TRUE(evaluated.outputs[0].value.has_value());
  EXPECT_EQ(std::get<std::uint64_t>(*evaluated.outputs[0].value), 15);
  ASSERT_TRUE(evaluated.outputs[1].value.has_value());
  EXPECT_DOUBLE_EQ(std::get<double>(*evaluated.outputs[1].value), 1500000000.0);
  EXPECT_EQ(evaluated.outputs[0].derivation.input_sequences,
            (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(evaluated.outputs[0].derivation.depth, 1);
}

TEST(CounterProcessorTest, AppliesOnlyExplicitWrapRule) {
  CounterProcessorConfig config = counterConfig();
  config.wrap = CounterWrapRule{256, 240, 15};
  const CounterDeltaRateProcessor processor(config);
  const ProcessorEvaluation wrapped = processor.evaluate(
      processorContext(20), ObservationSnapshot{{counterSample(250, 10, 1),
                                                 counterSample(5, 20, 2)}});
  ASSERT_TRUE(wrapped.status.ok());
  EXPECT_EQ(std::get<std::uint64_t>(*wrapped.outputs[0].value), 11);

  config.wrap.reset();
  const CounterDeltaRateProcessor no_wrap(config);
  const ProcessorEvaluation reset = no_wrap.evaluate(
      processorContext(20), ObservationSnapshot{{counterSample(250, 10, 1),
                                                 counterSample(5, 20, 2)}});
  EXPECT_EQ(reset.status.code(), PDCM_STATUS_PARTIAL_RESULT);
  EXPECT_EQ(reset.outputs[0].status, ObservationStatus::kNotAvailable);
  EXPECT_FALSE(reset.outputs[0].value.has_value());
}

TEST(CounterProcessorTest, RejectsEpochTimeAndGapDiscontinuities) {
  const CounterDeltaRateProcessor processor(counterConfig());

  ProcessorEvaluation changed_epoch = processor.evaluate(
      processorContext(20), ObservationSnapshot{{counterSample(10, 10, 1, 1),
                                                 counterSample(20, 20, 2, 2)}});
  EXPECT_EQ(changed_epoch.outputs[0].status, ObservationStatus::kNotAvailable);

  ProcessorEvaluation invalid_time = processor.evaluate(
      processorContext(10), ObservationSnapshot{{counterSample(10, 10, 1),
                                                 counterSample(20, 10, 2)}});
  EXPECT_EQ(invalid_time.outputs[0].status, ObservationStatus::kNotAvailable);

  ProcessorEvaluation gap = processor.evaluate(
      processorContext(200), ObservationSnapshot{{counterSample(10, 10, 1),
                                                  counterSample(20, 200, 2)}});
  EXPECT_EQ(gap.outputs[0].status, ObservationStatus::kNotAvailable);
}

TEST(ProcessorExecutorTest, DeduplicatesAndCommitsDerivedBatch) {
  TargetCatalog target = counterCatalog();
  SemanticCatalog semantic(target);
  ASSERT_TRUE(semantic.commit(providerDescriptor()).status.ok());
  const std::shared_ptr<const CatalogView> view = semantic.snapshot();
  const EntityRef entity = view->entities().front().ref;

  DataManager data;
  ASSERT_TRUE(data.activateCatalog(view, target).ok());
  Observation first = counterSample(10, 10, 0);
  Observation second = counterSample(25, 20, 0);
  first.entity = entity;
  second.entity = entity;
  ASSERT_TRUE(data.commit({first}).status.ok());
  ASSERT_TRUE(data.commit({second}).status.ok());
  const DataHistoryResult history = data.history(DataKey{entity, MetricId{1}});
  ASSERT_TRUE(history.status.ok());
  ASSERT_EQ(history.samples.size(), 2);

  ProcessorExecutor executor(data);
  ProcessorContext context{entity, view->generation(), 20, 1};
  ObservationSnapshot snapshot{history.samples};
  const ProcessorScheduleResult scheduled =
      executor.schedule(counterProcessor(), context, snapshot);
  ASSERT_TRUE(scheduled.status.ok());
  EXPECT_TRUE(scheduled.scheduled);
  const ProcessorScheduleResult duplicate =
      executor.schedule(counterProcessor(), context, snapshot);
  ASSERT_TRUE(duplicate.status.ok());
  EXPECT_FALSE(duplicate.scheduled);

  const ProcessorRunResult ran = executor.runNext();
  ASSERT_TRUE(ran.status.ok());
  EXPECT_TRUE(ran.ran);
  EXPECT_EQ(ran.commit.committed_items, 2);
  const DataReadResult derived = data.readLatest(
      {DataKey{entity, MetricId{2}}, DataKey{entity, MetricId{3}}}, false, 20,
      view->generation());
  ASSERT_TRUE(derived.status.ok());
  ASSERT_EQ(derived.items.size(), 2);
  EXPECT_EQ(std::get<std::uint64_t>(*derived.items[0].value), 15);
  EXPECT_DOUBLE_EQ(std::get<double>(*derived.items[1].value), 1500000000.0);

  const ProcessorScheduleResult recently_seen =
      executor.schedule(counterProcessor(), context, snapshot);
  ASSERT_TRUE(recently_seen.status.ok());
  EXPECT_FALSE(recently_seen.scheduled);
}

TEST(ProcessorExecutorTest, BoundsQueueAndDiscardsOldCatalogTask) {
  TargetCatalog target = counterCatalog();
  SemanticCatalog semantic(target);
  ASSERT_TRUE(semantic.commit(providerDescriptor()).status.ok());
  std::shared_ptr<const CatalogView> view = semantic.snapshot();
  const EntityRef entity = view->entities().front().ref;
  DataManager data;
  ASSERT_TRUE(data.activateCatalog(view, target).ok());

  ProcessorExecutorLimits limits;
  limits.max_queued_tasks = 1;
  ProcessorExecutor executor(data, limits);
  Observation first = counterSample(10, 10, 1);
  Observation second = counterSample(20, 20, 2);
  first.entity = entity;
  second.entity = entity;
  ObservationSnapshot snapshot{{first, second}};
  ProcessorContext context{entity, view->generation(), 20, 1};
  ASSERT_TRUE(
      executor.schedule(counterProcessor(), context, snapshot).scheduled);

  Observation newer = second;
  newer.commit_epoch = 3;
  newer.observed_monotonic_time_ns = 30;
  EXPECT_EQ(executor
                .schedule(counterProcessor(), context,
                          ObservationSnapshot{{second, newer}})
                .status.code(),
            PDCM_STATUS_RESOURCE_EXHAUSTED);

  ASSERT_TRUE(semantic.commit(providerDescriptor("pdrv-2")).status.ok());
  view = semantic.snapshot();
  ASSERT_TRUE(data.activateCatalog(view, target).ok());
  const ProcessorRunResult stale = executor.runNext();
  EXPECT_TRUE(stale.ran);
  EXPECT_TRUE(stale.discarded);
  EXPECT_EQ(stale.status.code(), PDCM_STATUS_STALE_GENERATION);
  EXPECT_EQ(data.keyCount(), 0);
}

} // namespace
} // namespace pdcm
