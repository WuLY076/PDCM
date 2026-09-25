#include "pdcm/testkit/mock_provider.hpp"

#include <stdexcept>
#include <utility>

#include "pdcm/testkit/mock_metric_ids.hpp"

namespace pdcm::testkit {
namespace {

ProviderEntity makeDevice(const std::uint32_t index) {
  ProviderEntity entity;
  entity.stable_native_id = "mock-device-" + std::to_string(index);
  entity.pci_bdf = index == 0 ? "0000:01:00.0" : "0000:02:00.0";
  entity.pdrv_version = "mock-pdrv-1.0";
  entity.incarnation = "mock-boot-1";
  entity.state = ProviderEntityState::kReady;
  entity.manageable = true;
  return entity;
}

ProviderCapability makeCapability(const ProviderDataKind kind,
                                  const std::uint32_t id) {
  ProviderCapability capability;
  capability.kind = kind;
  capability.data_id = id;
  capability.supported = true;
  capability.reason = "MOCK_SUPPORTED";
  return capability;
}

ProviderReadItemResult makeItemFailure(const ProviderReadItem &item,
                                       const ObservationStatus status,
                                       const std::int64_t native_code,
                                       const bool retryable) {
  ProviderReadItemResult result;
  result.item = item;
  result.status = status;
  result.native_source = "mock-script";
  result.native_code = native_code;
  result.retryable = retryable;
  return result;
}

} // namespace

MockProvider::MockProvider(MockProviderConfig config,
                           std::shared_ptr<const Clock> clock)
    : config_(config), clock_(std::move(clock)) {
  if (!clock_) {
    throw std::invalid_argument("MockProvider requires a clock");
  }
}

Status MockProvider::initialize(const ProviderInitOptions &options) {
  if (state_.load() == ProviderState::kShutdown) {
    return Status(PDCM_STATUS_NOT_INITIALIZED,
                  "shutdown provider cannot be initialized again");
  }
  if (options.target != TargetKind::kUnknown &&
      options.target != config_.target) {
    state_.store(ProviderState::kUnavailable);
    return Status(PDCM_STATUS_UNSUPPORTED, "mock target mismatch");
  }
  if (clock_->monotonicNow() > options.deadline) {
    state_.store(ProviderState::kUnavailable);
    return Status(PDCM_STATUS_TIMEOUT, "mock initialization deadline expired");
  }
  if (config_.scenario == MockScenario::kProviderUnavailable) {
    state_.store(ProviderState::kUnavailable);
    return Status(PDCM_STATUS_UNAVAILABLE, "scripted provider unavailable");
  }

  state_.store(ProviderState::kReady);
  return Status::success();
}

void MockProvider::shutdown() noexcept {
  state_.store(ProviderState::kShutdown);
}

ProviderState MockProvider::state() const noexcept { return state_.load(); }

ProviderConcurrency MockProvider::concurrency() const noexcept {
  return ProviderConcurrency{1, false};
}

ProviderDiscoveryResult MockProvider::discover(const MonotonicTime deadline) {
  ProviderDiscoveryResult result;
  result.descriptor = descriptor();

  if (state() != ProviderState::kReady) {
    result.call_status =
        Status(PDCM_STATUS_UNAVAILABLE, "provider is not ready");
    return result;
  }
  if (clock_->monotonicNow() > deadline) {
    result.call_status =
        Status(PDCM_STATUS_TIMEOUT, "discovery deadline expired");
    return result;
  }

  result.call_status = Status::success();
  return result;
}

ProviderReadResult MockProvider::batchRead(const ProviderReadRequest &request) {
  std::lock_guard<std::mutex> lock(read_mutex_);

  ProviderReadResult result;
  if (state() != ProviderState::kReady) {
    result.call_status =
        Status(PDCM_STATUS_UNAVAILABLE, "provider is not ready");
    return result;
  }

  if (clock_->monotonicNow() > request.deadline) {
    result.call_status = Status(PDCM_STATUS_TIMEOUT, "read deadline expired");
    result.items.reserve(request.items.size());
    for (const ProviderReadItem &item : request.items) {
      result.items.push_back(
          makeItemFailure(item, ObservationStatus::kError, 1001, true));
    }
    return result;
  }

  ++sample_sequence_;
  result.items.reserve(request.items.size());

  std::size_t valid_count = 0;
  std::size_t unsupported_count = 0;
  for (const ProviderReadItem &item : request.items) {
    ProviderReadItemResult item_result = readItem(item, sample_sequence_);
    if (item_result.status == ObservationStatus::kValid) {
      ++valid_count;
    } else if (item_result.status == ObservationStatus::kUnsupported) {
      ++unsupported_count;
    }
    result.items.push_back(std::move(item_result));
  }

  if (valid_count == result.items.size()) {
    result.call_status = Status::success();
  } else if (valid_count > 0) {
    result.call_status =
        Status(PDCM_STATUS_PARTIAL_RESULT, "scripted partial result");
  } else if (!result.items.empty() &&
             unsupported_count == result.items.size()) {
    result.call_status =
        Status(PDCM_STATUS_UNSUPPORTED, "all requested data is unsupported");
  } else if (result.items.empty()) {
    result.call_status =
        Status(PDCM_STATUS_INVALID_ARGUMENT, "empty read request");
  } else {
    result.call_status =
        Status(PDCM_STATUS_INTERNAL, "all scripted reads failed");
  }

  return result;
}

ProviderDescriptor MockProvider::descriptor() const {
  ProviderDescriptor value;
  value.provider_version = "mock-provider-1";
  value.target = config_.target;
  value.state = state();

  if (config_.scenario == MockScenario::kProviderUnavailable) {
    value.failure_phase = ProviderFailurePhase::kNativeInitialize;
    value.errors.emplace_back(PDCM_STATUS_UNAVAILABLE,
                              "scripted provider unavailable");
    return value;
  }

  if (config_.scenario == MockScenario::kZeroDevice) {
    return value;
  }

  const std::uint32_t device_count =
      config_.scenario == MockScenario::kMultipleDevices ? 2U : 1U;
  value.detected_device_count = device_count;
  value.entities.reserve(device_count);
  for (std::uint32_t index = 0; index < device_count; ++index) {
    value.entities.push_back(makeDevice(index));
  }

  value.capabilities = {
      makeCapability(ProviderDataKind::kMetric, kTestGaugeMetricId),
      makeCapability(ProviderDataKind::kMetric, kTestCounterMetricId),
      makeCapability(ProviderDataKind::kMetric, kTestPeriodicFailureMetricId),
      makeCapability(ProviderDataKind::kHeartbeatEvidence,
                     kTestHeartbeatEvidenceId),
  };
  return value;
}

ProviderReadItemResult
MockProvider::readItem(const ProviderReadItem &item,
                       const std::uint64_t sequence) const {
  if (item.entity.kind != EntityKind::kDevice ||
      item.entity.id != EntityId{0} || item.entity.generation != 1) {
    return makeItemFailure(item, ObservationStatus::kError, 1002, false);
  }

  ProviderReadItemResult result;
  result.item = item;
  result.status = ObservationStatus::kValid;
  result.source_sample_time_ns =
      clock_->monotonicNow().time_since_epoch().count();
  result.native_source = "mock-script";

  if (item.kind == ProviderDataKind::kHeartbeatEvidence &&
      item.data_id == kTestHeartbeatEvidenceId) {
    switch (config_.heartbeat) {
    case MockHeartbeatClass::kNormal:
      result.heartbeat_class = HeartbeatNativeClass::kNormal;
      break;
    case MockHeartbeatClass::kWarning:
      result.heartbeat_class = HeartbeatNativeClass::kWarning;
      break;
    case MockHeartbeatClass::kFault:
      result.heartbeat_class = HeartbeatNativeClass::kFault;
      break;
    }
    result.sequence_or_token = sequence;
    return result;
  }

  if (item.kind != ProviderDataKind::kMetric) {
    return makeItemFailure(item, ObservationStatus::kUnsupported, 0, false);
  }

  switch (item.data_id) {
  case kTestGaugeMetricId:
    result.value = sequence * 10U;
    return result;
  case kTestCounterMetricId:
    result.value = sequence * 100U;
    return result;
  case kTestPeriodicFailureMetricId:
    if (config_.scenario == MockScenario::kPartialMetricFailure) {
      return makeItemFailure(item, ObservationStatus::kError, 2001, true);
    }
    result.value = sequence;
    return result;
  case kTestDerivedRateMetricId:
    return makeItemFailure(item, ObservationStatus::kUnsupported, 0, false);
  default:
    return makeItemFailure(item, ObservationStatus::kUnsupported, 0, false);
  }
}

} // namespace pdcm::testkit
