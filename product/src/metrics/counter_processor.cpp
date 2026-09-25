#include "metrics/counter_processor.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <variant>

namespace pdcm {

Status CounterWrapRule::validate() const {
  if (modulus < 2 || minimum_previous >= modulus ||
      maximum_current >= modulus || minimum_previous <= maximum_current) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT, "counter wrap rule is invalid");
  }
  return Status::success();
}

Status CounterProcessorConfig::validate() const {
  if (processor_id.empty() || processor_version == 0 ||
      input.metric.value == 0 || input.semantic_version == 0 ||
      delta_output.metric.value == 0 || delta_output.semantic_version == 0 ||
      input.metric == delta_output.metric || minimum_interval_ns <= 0 ||
      maximum_gap_ns < minimum_interval_ns) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "counter processor configuration is invalid");
  }
  if (rate_output.has_value() &&
      (rate_output->metric.value == 0 || rate_output->semantic_version == 0 ||
       rate_output->metric == input.metric ||
       rate_output->metric == delta_output.metric)) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "counter rate output is invalid");
  }
  if (wrap.has_value()) {
    return wrap->validate();
  }
  return Status::success();
}

CounterDeltaRateProcessor::CounterDeltaRateProcessor(
    CounterProcessorConfig config)
    : config_(std::move(config)) {
  const Status status = config_.validate();
  if (!status.ok()) {
    throw std::invalid_argument(status.message());
  }
}

std::string CounterDeltaRateProcessor::id() const {
  return config_.processor_id;
}

std::uint32_t CounterDeltaRateProcessor::version() const noexcept {
  return config_.processor_version;
}

std::vector<ProcessorMetric> CounterDeltaRateProcessor::inputs() const {
  return {config_.input};
}

std::vector<ProcessorMetric> CounterDeltaRateProcessor::outputs() const {
  std::vector<ProcessorMetric> result{config_.delta_output};
  if (config_.rate_output.has_value()) {
    result.push_back(*config_.rate_output);
  }
  return result;
}

ProcessorEvaluation
CounterDeltaRateProcessor::evaluate(const ProcessorContext &context,
                                    const ObservationSnapshot &snapshot) const {
  if (context.entity.kind != EntityKind::kDevice ||
      context.entity.generation == 0 || context.catalog_generation == 0 ||
      context.evaluated_monotonic_time_ns < 0 ||
      context.derivation_depth == 0 || context.derivation_depth > 4 ||
      snapshot.items.size() != 2) {
    return unavailable(
        context, snapshot.items.empty() ? nullptr : &snapshot.items.back(),
        "insufficient samples");
  }

  std::vector<const Observation *> samples;
  samples.reserve(snapshot.items.size());
  for (const Observation &item : snapshot.items) {
    samples.push_back(&item);
  }
  std::sort(
      samples.begin(), samples.end(),
      [](const Observation *lhs, const Observation *rhs) {
        return std::tie(lhs->observed_monotonic_time_ns, lhs->commit_epoch) <
               std::tie(rhs->observed_monotonic_time_ns, rhs->commit_epoch);
      });
  const Observation &oldest = *samples.front();
  const Observation &newest = *samples.back();

  if (oldest.entity != context.entity || newest.entity != context.entity ||
      oldest.catalog_generation != context.catalog_generation ||
      newest.catalog_generation != context.catalog_generation ||
      oldest.metric != config_.input.metric ||
      newest.metric != config_.input.metric ||
      oldest.metric_semantic_version != config_.input.semantic_version ||
      newest.metric_semantic_version != config_.input.semantic_version ||
      oldest.status != ObservationStatus::kValid ||
      newest.status != ObservationStatus::kValid || !oldest.value.has_value() ||
      !newest.value.has_value() ||
      !std::holds_alternative<std::uint64_t>(*oldest.value) ||
      !std::holds_alternative<std::uint64_t>(*newest.value)) {
    return unavailable(context, &newest,
                       "counter samples are invalid or changed generation");
  }
  if (oldest.counter_epoch != newest.counter_epoch) {
    return unavailable(context, &newest, "counter epoch changed");
  }
  if (newest.observed_monotonic_time_ns <= oldest.observed_monotonic_time_ns ||
      context.evaluated_monotonic_time_ns < newest.observed_monotonic_time_ns) {
    return unavailable(context, &newest, "counter sample time is invalid");
  }
  const std::int64_t interval =
      newest.observed_monotonic_time_ns - oldest.observed_monotonic_time_ns;
  if (interval < config_.minimum_interval_ns) {
    return unavailable(context, &newest, "counter interval is too small");
  }
  if (interval > config_.maximum_gap_ns) {
    return unavailable(context, &newest, "counter gap is too large");
  }

  const std::uint64_t old_value = std::get<std::uint64_t>(*oldest.value);
  const std::uint64_t new_value = std::get<std::uint64_t>(*newest.value);
  std::uint64_t delta = 0;
  if (new_value >= old_value) {
    delta = new_value - old_value;
  } else if (config_.wrap.has_value() &&
             old_value >= config_.wrap->minimum_previous &&
             old_value < config_.wrap->modulus &&
             new_value <= config_.wrap->maximum_current) {
    delta = (config_.wrap->modulus - old_value) + new_value;
  } else {
    return unavailable(context, &newest, "counter reset was detected");
  }

  ProcessorEvaluation result;
  Observation delta_output =
      outputBase(context, oldest, newest, config_.delta_output);
  delta_output.value = delta;
  delta_output.status = ObservationStatus::kValid;
  result.outputs.push_back(std::move(delta_output));
  if (config_.rate_output.has_value()) {
    Observation rate_output =
        outputBase(context, oldest, newest, *config_.rate_output);
    rate_output.value = static_cast<double>(delta) * 1000000000.0 /
                        static_cast<double>(interval);
    rate_output.status = ObservationStatus::kValid;
    result.outputs.push_back(std::move(rate_output));
  }
  result.status = Status::success();
  return result;
}

ProcessorEvaluation
CounterDeltaRateProcessor::unavailable(const ProcessorContext &context,
                                       const Observation *const newest,
                                       const char *const reason) const {
  ProcessorEvaluation result;
  for (const ProcessorMetric &output : outputs()) {
    Observation item;
    item.entity = context.entity;
    item.metric = output.metric;
    item.status = ObservationStatus::kNotAvailable;
    item.metric_semantic_version = output.semantic_version;
    item.scheduled_monotonic_time_ns =
        newest == nullptr ? context.evaluated_monotonic_time_ns
                          : newest->scheduled_monotonic_time_ns;
    item.observed_monotonic_time_ns = context.evaluated_monotonic_time_ns;
    item.observed_wall_time_ns =
        newest == nullptr ? 0 : newest->observed_wall_time_ns;
    item.catalog_generation = context.catalog_generation;
    item.counter_epoch = newest == nullptr ? 0 : newest->counter_epoch;
    item.source.provider = "pdcm-processor";
    item.source.native_source = config_.processor_id;
    item.error.status = Status(PDCM_STATUS_UNAVAILABLE, reason);
    item.derivation.processor_id = config_.processor_id;
    item.derivation.processor_version = config_.processor_version;
    item.derivation.depth = context.derivation_depth;
    item.derivation.input_sequences.push_back(
        newest == nullptr ? 1 : newest->commit_epoch);
    result.outputs.push_back(std::move(item));
  }
  result.status =
      Status(PDCM_STATUS_PARTIAL_RESULT, "counter output is not available");
  return result;
}

Observation CounterDeltaRateProcessor::outputBase(
    const ProcessorContext &context, const Observation &oldest,
    const Observation &newest, const ProcessorMetric &output) const {
  Observation result;
  result.entity = context.entity;
  result.metric = output.metric;
  result.scheduled_monotonic_time_ns = newest.scheduled_monotonic_time_ns;
  result.source_sample_time_ns = newest.source_sample_time_ns;
  result.observed_monotonic_time_ns = context.evaluated_monotonic_time_ns;
  result.metric_semantic_version = output.semantic_version;
  result.observed_wall_time_ns = newest.observed_wall_time_ns;
  result.catalog_generation = context.catalog_generation;
  result.counter_epoch = newest.counter_epoch;
  result.source.provider = "pdcm-processor";
  result.source.native_source = config_.processor_id;
  result.derivation.processor_id = config_.processor_id;
  result.derivation.input_sequences = {oldest.commit_epoch,
                                       newest.commit_epoch};
  result.derivation.processor_version = config_.processor_version;
  result.derivation.depth = context.derivation_depth;
  return result;
}

} // namespace pdcm
