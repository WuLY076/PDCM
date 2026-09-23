#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>
#include <variant>

#include "pdcm/testkit/manual_clock.hpp"
#include "pdcm/testkit/mock_metric_ids.hpp"
#include "pdcm/testkit/mock_provider.hpp"

namespace {

int failures = 0;

void check(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}

pdcm::ProviderInitOptions initOptions(const pdcm::testkit::ManualClock &clock) {
  pdcm::ProviderInitOptions options;
  options.target = pdcm::TargetKind::kFpga;
  options.deadline = clock.monotonicNow() + std::chrono::seconds(1);
  return options;
}

pdcm::ProviderReadItem readItem(const pdcm::ProviderDataKind kind,
                                const std::uint32_t id) {
  pdcm::ProviderReadItem item;
  item.entity =
      pdcm::EntityRef{pdcm::EntityKind::kDevice, pdcm::EntityId{0}, 1};
  item.kind = kind;
  item.data_id = id;
  return item;
}

pdcm::ProviderReadRequest readRequest(const pdcm::testkit::ManualClock &clock) {
  pdcm::ProviderReadRequest request;
  request.request_id = 1;
  request.plan_generation = 1;
  request.catalog_generation = 1;
  request.scheduled_time = clock.monotonicNow();
  request.deadline = clock.monotonicNow() + std::chrono::seconds(1);
  return request;
}

} // namespace

int main() {
  using namespace pdcm;
  using namespace pdcm::testkit;

  auto clock = std::make_shared<ManualClock>(100, 1000);

  MockProvider single(MockProviderConfig{}, clock);
  check(single.initialize(initOptions(*clock)).ok(),
        "single-device provider initializes");
  check(single.state() == ProviderState::kReady,
        "initialized provider is ready");
  check(single.concurrency().max_concurrency == 1,
        "mock provider declares serialized calls");

  ProviderDiscoveryResult discovery =
      single.discover(clock->monotonicNow() + std::chrono::seconds(1));
  check(discovery.call_status.ok(), "single-device discovery succeeds");
  check(discovery.descriptor.detected_device_count == 1,
        "single-device discovery reports one device");
  check(discovery.descriptor.entities.size() == 1,
        "single-device descriptor has one entity");
  check(discovery.descriptor.capabilities.size() == 4,
        "mock capabilities are explicit");

  ProviderReadRequest request = readRequest(*clock);
  request.items = {
      readItem(ProviderDataKind::kMetric, kTestGaugeMetricId),
      readItem(ProviderDataKind::kMetric, kTestCounterMetricId),
      readItem(ProviderDataKind::kHeartbeatEvidence, kTestHeartbeatEvidenceId),
  };
  ProviderReadResult first = single.batchRead(request);
  check(first.call_status.ok(), "supported batch read succeeds");
  check(first.items.size() == request.items.size(),
        "batch result contains every requested item");
  check(std::get<std::uint64_t>(*first.items[0].value) == 10,
        "gauge value is deterministic");
  check(std::get<std::uint64_t>(*first.items[1].value) == 100,
        "counter value is deterministic");
  check(std::get<std::uint64_t>(*first.items[2].value) == 0,
        "normal heartbeat class is deterministic");

  ProviderReadResult second = single.batchRead(request);
  check(std::get<std::uint64_t>(*second.items[0].value) == 20,
        "sample sequence advances once per batch");

  MockProviderConfig partial_config;
  partial_config.scenario = MockScenario::kPartialMetricFailure;
  MockProvider partial(partial_config, clock);
  check(partial.initialize(initOptions(*clock)).ok(),
        "partial provider initializes");

  ProviderReadRequest partial_request = readRequest(*clock);
  partial_request.items = {
      readItem(ProviderDataKind::kMetric, kTestGaugeMetricId),
      readItem(ProviderDataKind::kMetric, kTestPeriodicFailureMetricId),
  };
  ProviderReadResult partial_result = partial.batchRead(partial_request);
  check(partial_result.call_status.code() == PDCM_STATUS_PARTIAL_RESULT,
        "mixed item result reports partial");
  check(partial_result.items[0].status == ObservationStatus::kValid,
        "successful item is retained");
  check(partial_result.items[1].status == ObservationStatus::kError,
        "scripted failure remains item-level");
  check(!partial_result.items[1].value.has_value(),
        "failed item has no fabricated value");

  MockProviderConfig zero_config;
  zero_config.scenario = MockScenario::kZeroDevice;
  MockProvider zero(zero_config, clock);
  check(zero.initialize(initOptions(*clock)).ok(),
        "zero-device provider initializes");
  check(zero.discover(clock->monotonicNow() + std::chrono::seconds(1))
                .descriptor.detected_device_count == 0,
        "zero-device discovery is explicit");

  MockProviderConfig multiple_config;
  multiple_config.scenario = MockScenario::kMultipleDevices;
  MockProvider multiple(multiple_config, clock);
  check(multiple.initialize(initOptions(*clock)).ok(),
        "multiple-device provider initializes");
  ProviderDiscoveryResult multiple_result =
      multiple.discover(clock->monotonicNow() + std::chrono::seconds(1));
  check(multiple_result.descriptor.detected_device_count == 2,
        "multiple-device fixture exposes detected count");
  check(multiple_result.descriptor.entities.size() == 2,
        "provider does not silently discard the second device");

  MockProviderConfig unavailable_config;
  unavailable_config.scenario = MockScenario::kProviderUnavailable;
  MockProvider unavailable(unavailable_config, clock);
  check(unavailable.initialize(initOptions(*clock)).code() ==
            PDCM_STATUS_UNAVAILABLE,
        "unavailable fixture fails initialization explicitly");
  check(unavailable.state() == ProviderState::kUnavailable,
        "unavailable fixture publishes state");

  ProviderReadRequest expired = readRequest(*clock);
  expired.items = {
      readItem(ProviderDataKind::kMetric, kTestGaugeMetricId),
  };
  clock->advance(std::chrono::seconds(2));
  ProviderReadResult timeout = single.batchRead(expired);
  check(timeout.call_status.code() == PDCM_STATUS_TIMEOUT,
        "expired read returns timeout");
  check(timeout.items.size() == 1 &&
            timeout.items[0].status == ObservationStatus::kError,
        "timeout preserves an item-level result");

  single.shutdown();
  check(single.state() == ProviderState::kShutdown,
        "shutdown is externally visible");
  single.shutdown();
  check(single.batchRead(readRequest(*clock)).call_status.code() ==
            PDCM_STATUS_UNAVAILABLE,
        "shutdown provider rejects new reads");

  bool backwards_rejected = false;
  try {
    clock->advance(std::chrono::nanoseconds(-1));
  } catch (const std::invalid_argument &) {
    backwards_rejected = true;
  }
  check(backwards_rejected, "manual clock rejects backward movement");

  return failures == 0 ? 0 : 1;
}
