#ifndef PDCM_METRICS_COUNTER_PROCESSOR_HPP_
#define PDCM_METRICS_COUNTER_PROCESSOR_HPP_

#include <cstdint>
#include <optional>
#include <string>

#include "metrics/processor.hpp"

namespace pdcm {

struct CounterWrapRule {
  std::uint64_t modulus{0};
  std::uint64_t minimum_previous{0};
  std::uint64_t maximum_current{0};

  [[nodiscard]] Status validate() const;
};

struct CounterProcessorConfig {
  std::string processor_id;
  std::uint32_t processor_version{0};
  ProcessorMetric input;
  ProcessorMetric delta_output;
  std::optional<ProcessorMetric> rate_output;
  std::int64_t minimum_interval_ns{0};
  std::int64_t maximum_gap_ns{0};
  std::optional<CounterWrapRule> wrap;

  [[nodiscard]] Status validate() const;
};

class CounterDeltaRateProcessor final : public MetricProcessor {
public:
  explicit CounterDeltaRateProcessor(CounterProcessorConfig config);

  [[nodiscard]] std::string id() const override;
  [[nodiscard]] std::uint32_t version() const noexcept override;
  [[nodiscard]] std::vector<ProcessorMetric> inputs() const override;
  [[nodiscard]] std::vector<ProcessorMetric> outputs() const override;
  [[nodiscard]] ProcessorEvaluation
  evaluate(const ProcessorContext &context,
           const ObservationSnapshot &snapshot) const override;

private:
  [[nodiscard]] ProcessorEvaluation unavailable(const ProcessorContext &context,
                                                const Observation *newest,
                                                const char *reason) const;
  [[nodiscard]] Observation outputBase(const ProcessorContext &context,
                                       const Observation &oldest,
                                       const Observation &newest,
                                       const ProcessorMetric &output) const;

  CounterProcessorConfig config_;
};

} // namespace pdcm

#endif // PDCM_METRICS_COUNTER_PROCESSOR_HPP_
