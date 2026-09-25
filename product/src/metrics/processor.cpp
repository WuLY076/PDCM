#include "metrics/processor.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace pdcm {
namespace {

bool readableProcessorStatus(const Status &status) {
  return status.ok() || status.code() == PDCM_STATUS_PARTIAL_RESULT;
}

std::size_t observationBytes(const Observation &observation) {
  std::size_t bytes =
      sizeof(Observation) + observation.source.provider.size() +
      observation.source.native_source.size() +
      observation.error.status.message().size() +
      observation.derivation.processor_id.size() +
      observation.derivation.input_sequences.size() * sizeof(std::uint64_t);
  if (observation.value.has_value()) {
    bytes += std::visit(
        [](const auto &value) -> std::size_t {
          using Value = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, std::string>) {
            return value.size();
          } else {
            return sizeof(Value);
          }
        },
        *observation.value);
  }
  return bytes;
}

} // namespace

ProcessorGraph::ProcessorGraph()
    : snapshot_(std::make_shared<const ProcessorGraphSnapshot>()) {}

Status ProcessorGraph::rebuild(
    const TargetCatalog &catalog, const std::uint64_t catalog_generation,
    const std::vector<std::shared_ptr<const MetricProcessor>> &processors) {
  const Status catalog_status = catalog.validate();
  if (!catalog_status.ok() || catalog_generation == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "processor graph catalog is invalid");
  }

  std::map<std::uint32_t, MetricDescriptor> descriptors;
  for (const MetricDescriptor &descriptor : catalog.metrics) {
    descriptors.emplace(descriptor.id.value, descriptor);
  }

  std::set<std::string> processor_ids;
  std::map<std::uint32_t, std::size_t> producers;
  std::vector<std::vector<ProcessorMetric>> inputs;
  std::vector<std::vector<ProcessorMetric>> outputs;
  inputs.reserve(processors.size());
  outputs.reserve(processors.size());

  for (std::size_t index = 0; index < processors.size(); ++index) {
    const std::shared_ptr<const MetricProcessor> &processor = processors[index];
    if (!processor || processor->id().empty() || processor->version() == 0 ||
        !processor_ids.insert(processor->id()).second) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "processor identity is invalid or duplicated");
    }
    std::vector<ProcessorMetric> processor_inputs = processor->inputs();
    std::vector<ProcessorMetric> processor_outputs = processor->outputs();
    if (processor_inputs.empty() || processor_outputs.empty()) {
      return Status(PDCM_STATUS_INVALID_ARGUMENT,
                    "processor inputs and outputs must be non-empty");
    }

    std::set<std::uint32_t> input_ids;
    for (const ProcessorMetric &input : processor_inputs) {
      const auto descriptor = descriptors.find(input.metric.value);
      if (input.metric.value == 0 || input.semantic_version == 0 ||
          !input_ids.insert(input.metric.value).second ||
          descriptor == descriptors.end() ||
          descriptor->second.semantic_version != input.semantic_version) {
        return Status(
            PDCM_STATUS_UNSUPPORTED,
            "processor input descriptor or semantic version is missing");
      }
    }

    std::set<std::uint32_t> output_ids;
    for (const ProcessorMetric &output : processor_outputs) {
      const auto descriptor = descriptors.find(output.metric.value);
      if (output.metric.value == 0 || output.semantic_version == 0 ||
          !output_ids.insert(output.metric.value).second ||
          descriptor == descriptors.end() ||
          descriptor->second.collection_mode !=
              MetricCollectionMode::kDerived ||
          descriptor->second.semantic_version != output.semantic_version) {
        return Status(PDCM_STATUS_UNSUPPORTED,
                      "processor output descriptor is missing or not derived");
      }
      if (!producers.emplace(output.metric.value, index).second) {
        return Status(PDCM_STATUS_INVALID_ARGUMENT,
                      "derived metric has multiple authoritative processors");
      }

      std::vector<std::uint32_t> declared_dependencies;
      for (const MetricId dependency : descriptor->second.dependencies) {
        declared_dependencies.push_back(dependency.value);
      }
      std::vector<std::uint32_t> processor_dependencies;
      for (const ProcessorMetric &input : processor_inputs) {
        processor_dependencies.push_back(input.metric.value);
      }
      std::sort(declared_dependencies.begin(), declared_dependencies.end());
      std::sort(processor_dependencies.begin(), processor_dependencies.end());
      if (declared_dependencies != processor_dependencies) {
        return Status(PDCM_STATUS_UNSUPPORTED,
                      "derived descriptor dependencies do not match processor");
      }
    }
    inputs.push_back(std::move(processor_inputs));
    outputs.push_back(std::move(processor_outputs));
  }

  for (const MetricDescriptor &descriptor : catalog.metrics) {
    if (descriptor.collection_mode == MetricCollectionMode::kDerived &&
        producers.find(descriptor.id.value) == producers.end()) {
      return Status(PDCM_STATUS_UNSUPPORTED,
                    "derived metric has no authoritative processor");
    }
  }

  std::vector<std::vector<std::size_t>> edges(processors.size());
  std::vector<std::size_t> indegree(processors.size(), 0);
  for (std::size_t consumer = 0; consumer < processors.size(); ++consumer) {
    std::set<std::size_t> predecessors;
    for (const ProcessorMetric &input : inputs[consumer]) {
      const auto producer = producers.find(input.metric.value);
      if (producer != producers.end() &&
          predecessors.insert(producer->second).second) {
        edges[producer->second].push_back(consumer);
        ++indegree[consumer];
      }
    }
  }

  std::set<std::pair<std::string, std::size_t>> ready;
  for (std::size_t index = 0; index < processors.size(); ++index) {
    if (indegree[index] == 0) {
      ready.emplace(processors[index]->id(), index);
    }
  }
  std::vector<std::uint8_t> depth(processors.size(), 1);
  auto next = std::make_shared<ProcessorGraphSnapshot>();
  next->catalog_generation = catalog_generation;
  next->nodes.reserve(processors.size());
  while (!ready.empty()) {
    const std::size_t index = ready.begin()->second;
    ready.erase(ready.begin());
    if (depth[index] > 4) {
      return Status(PDCM_STATUS_UNSUPPORTED,
                    "processor derivation depth exceeds four");
    }
    next->nodes.push_back(ProcessorGraphNode{processors[index], depth[index]});
    for (const std::size_t consumer : edges[index]) {
      depth[consumer] = std::max(depth[consumer],
                                 static_cast<std::uint8_t>(depth[index] + 1U));
      --indegree[consumer];
      if (indegree[consumer] == 0) {
        ready.emplace(processors[consumer]->id(), consumer);
      }
    }
  }
  if (next->nodes.size() != processors.size()) {
    return Status(PDCM_STATUS_UNSUPPORTED,
                  "processor dependency graph contains a cycle");
  }

  std::lock_guard<std::mutex> lock(writer_mutex_);
  const std::shared_ptr<const ProcessorGraphSnapshot> current = snapshot();
  if (current->catalog_generation > catalog_generation) {
    return Status(PDCM_STATUS_STALE_GENERATION,
                  "processor graph generation moved backward");
  }
  std::atomic_store_explicit(
      &snapshot_,
      std::shared_ptr<const ProcessorGraphSnapshot>(std::move(next)),
      std::memory_order_release);
  return Status::success();
}

std::shared_ptr<const ProcessorGraphSnapshot>
ProcessorGraph::snapshot() const noexcept {
  return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
}

Status ProcessorExecutorLimits::validate() const {
  if (max_queued_tasks == 0 || max_queue_bytes == 0 || max_recent_keys == 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "processor executor limits must be non-zero");
  }
  return Status::success();
}

ProcessorExecutor::ProcessorExecutor(DataManager &data_manager,
                                     ProcessorExecutorLimits limits)
    : data_manager_(data_manager), limits_(limits) {
  const Status status = limits_.validate();
  if (!status.ok()) {
    throw std::invalid_argument(status.message());
  }
}

ProcessorScheduleResult
ProcessorExecutor::schedule(std::shared_ptr<const MetricProcessor> processor,
                            ProcessorContext context,
                            ObservationSnapshot snapshot) {
  ProcessorScheduleResult result;
  if (!processor || processor->id().empty() || processor->version() == 0 ||
      context.entity.kind != EntityKind::kDevice ||
      context.entity.generation == 0 || context.catalog_generation == 0 ||
      context.evaluated_monotonic_time_ns < 0 ||
      context.derivation_depth == 0 || context.derivation_depth > 4 ||
      snapshot.items.empty()) {
    result.status =
        Status(PDCM_STATUS_INVALID_ARGUMENT, "processor task is malformed");
    return result;
  }
  for (const Observation &item : snapshot.items) {
    if (item.entity != context.entity ||
        item.catalog_generation != context.catalog_generation ||
        item.commit_epoch == 0 || !validateObservation(item).ok()) {
      result.status = Status(PDCM_STATUS_STALE_GENERATION,
                             "processor input snapshot is inconsistent");
      return result;
    }
  }

  const std::size_t bytes = taskBytes(*processor, snapshot);
  if (bytes > limits_.max_queue_bytes) {
    result.status = Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                           "processor task exceeds queue byte limit");
    return result;
  }
  const std::string key = dedupKey(*processor, context, snapshot);
  std::lock_guard<std::mutex> lock(mutex_);
  if (queued_keys_.find(key) != queued_keys_.end() ||
      recent_keys_.find(key) != recent_keys_.end()) {
    result.status = Status::success();
    return result;
  }
  if (queue_.size() >= limits_.max_queued_tasks ||
      bytes > limits_.max_queue_bytes - queued_bytes_) {
    result.status = Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                           "processor queue limit exceeded");
    return result;
  }
  queued_bytes_ += bytes;
  queued_keys_.insert(key);
  queue_.push_back(
      Task{std::move(processor), context, std::move(snapshot), key, bytes});
  result.status = Status::success();
  result.scheduled = true;
  return result;
}

ProcessorRunResult ProcessorExecutor::runNext() {
  std::lock_guard<std::mutex> serial(run_mutex_);
  ProcessorRunResult result;
  Task task;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      result.status = Status::success();
      return result;
    }
    task = std::move(queue_.front());
    queue_.pop_front();
    queued_bytes_ -= task.bytes;
    queued_keys_.erase(task.dedup_key);
  }
  result.ran = true;

  if (data_manager_.catalogGeneration() != task.context.catalog_generation) {
    remember(task.dedup_key);
    result.status =
        Status(PDCM_STATUS_STALE_GENERATION, "processor task catalog is stale");
    result.discarded = true;
    return result;
  }

  ProcessorEvaluation evaluation =
      task.processor->evaluate(task.context, task.snapshot);
  if (!readableProcessorStatus(evaluation.status)) {
    remember(task.dedup_key);
    result.status = evaluation.status;
    return result;
  }
  const std::vector<ProcessorMetric> expected = task.processor->outputs();
  if (evaluation.outputs.size() != expected.size()) {
    remember(task.dedup_key);
    result.status = Status(PDCM_STATUS_INTERNAL,
                           "processor output count violates contract");
    return result;
  }

  std::map<std::uint32_t, std::uint32_t> expected_versions;
  for (const ProcessorMetric &output : expected) {
    expected_versions.emplace(output.metric.value, output.semantic_version);
  }
  std::set<std::uint32_t> observed_outputs;
  for (const Observation &output : evaluation.outputs) {
    const auto expected_output = expected_versions.find(output.metric.value);
    if (expected_output == expected_versions.end() ||
        !observed_outputs.insert(output.metric.value).second ||
        output.entity != task.context.entity ||
        output.catalog_generation != task.context.catalog_generation ||
        output.commit_epoch != 0 ||
        output.metric_semantic_version != expected_output->second ||
        output.derivation.processor_id != task.processor->id() ||
        output.derivation.processor_version != task.processor->version() ||
        output.derivation.depth != task.context.derivation_depth ||
        !validateObservation(output).ok()) {
      remember(task.dedup_key);
      result.status =
          Status(PDCM_STATUS_INTERNAL, "processor output violates contract");
      return result;
    }
  }

  if (data_manager_.catalogGeneration() != task.context.catalog_generation) {
    remember(task.dedup_key);
    result.status = Status(PDCM_STATUS_STALE_GENERATION,
                           "processor output catalog became stale");
    result.discarded = true;
    return result;
  }
  result.commit = data_manager_.commit(evaluation.outputs);
  result.status = result.commit.status;
  remember(task.dedup_key);
  return result;
}

std::size_t ProcessorExecutor::queuedTasks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

std::size_t ProcessorExecutor::queuedBytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queued_bytes_;
}

std::size_t ProcessorExecutor::taskBytes(const MetricProcessor &processor,
                                         const ObservationSnapshot &snapshot) {
  std::size_t bytes = sizeof(Task) + processor.id().size();
  for (const Observation &item : snapshot.items) {
    const std::size_t item_bytes = observationBytes(item);
    if (bytes > std::numeric_limits<std::size_t>::max() - item_bytes) {
      return std::numeric_limits<std::size_t>::max();
    }
    bytes += item_bytes;
  }
  return bytes;
}

std::string ProcessorExecutor::dedupKey(const MetricProcessor &processor,
                                        const ProcessorContext &context,
                                        const ObservationSnapshot &snapshot) {
  std::vector<std::pair<std::uint32_t, std::uint64_t>> inputs;
  inputs.reserve(snapshot.items.size());
  for (const Observation &item : snapshot.items) {
    inputs.emplace_back(item.metric.value, item.commit_epoch);
  }
  std::sort(inputs.begin(), inputs.end());

  std::ostringstream key;
  key << processor.id() << ':'
      << static_cast<std::uint32_t>(context.entity.kind) << ':'
      << context.entity.id.value << ':' << context.entity.generation << ':'
      << context.catalog_generation;
  for (const auto &input : inputs) {
    key << ':' << input.first << '@' << input.second;
  }
  return key.str();
}

void ProcessorExecutor::remember(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!recent_keys_.insert(key).second) {
    return;
  }
  recent_order_.push_back(key);
  while (recent_order_.size() > limits_.max_recent_keys) {
    recent_keys_.erase(recent_order_.front());
    recent_order_.pop_front();
  }
}

} // namespace pdcm
