#include "core/service_core.hpp"

#include <stdexcept>
#include <utility>

namespace pdcm {

PdcmServiceCore::PdcmServiceCore(RuntimeConfig config,
                                 std::unique_ptr<Provider> provider,
                                 std::shared_ptr<const Clock> clock)
    : config_(std::move(config)), provider_manager_(std::move(provider)),
      clock_(std::move(clock)) {
  if (!clock_) {
    throw std::invalid_argument("PdcmServiceCore requires a clock");
  }
}

PdcmServiceCore::~PdcmServiceCore() { (void)stop(); }

Status PdcmServiceCore::start() {
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (snapshot_.state != CoreState::kCreated) {
      return Status(PDCM_STATUS_NOT_INITIALIZED,
                    "core can only be started once");
    }
    snapshot_.state = CoreState::kStarting;
  }

  const Status config_status = config_.validate();
  if (!config_status.ok()) {
    publish(CoreState::kFailed, ProviderState::kUninitialized,
            CoreDegradedReason::kNone, config_status.code(), 0, 0);
    return config_status;
  }

  ProviderInitOptions init_options;
  init_options.target = config_.target;
  init_options.deadline =
      clock_->monotonicNow() + config_.provider_call_timeout;

  const Status provider_status = provider_manager_.initialize(init_options);
  if (!provider_status.ok()) {
    publish(CoreState::kDegraded, provider_manager_.state(),
            CoreDegradedReason::kProviderUnavailable, provider_status.code(), 0,
            0);
    return Status::success();
  }

  const ProviderDiscoveryResult discovery = provider_manager_.discover(
      clock_->monotonicNow() + config_.provider_call_timeout);
  if (!discovery.call_status.ok()) {
    publish(CoreState::kDegraded, provider_manager_.state(),
            CoreDegradedReason::kDiscoveryFailed, discovery.call_status.code(),
            discovery.descriptor.detected_device_count, 0);
    return Status::success();
  }

  if (discovery.descriptor.detected_device_count > 1) {
    publish(CoreState::kDegraded, provider_manager_.state(),
            CoreDegradedReason::kTopologyUnsupported, PDCM_STATUS_UNSUPPORTED,
            discovery.descriptor.detected_device_count, 0);
    return Status::success();
  }

  publish(CoreState::kReady, provider_manager_.state(),
          CoreDegradedReason::kNone, PDCM_STATUS_SUCCESS,
          discovery.descriptor.detected_device_count, 1);
  return Status::success();
}

Status PdcmServiceCore::stop() noexcept {
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (snapshot_.state == CoreState::kStopped) {
      return Status::success();
    }
    snapshot_.state = CoreState::kStopping;
  }

  provider_manager_.shutdown();
  publish(CoreState::kStopped, ProviderState::kShutdown,
          CoreDegradedReason::kNone, PDCM_STATUS_SUCCESS, 0, 0);
  return Status::success();
}

CoreSnapshot PdcmServiceCore::snapshot() const {
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  return snapshot_;
}

const RuntimeConfig &PdcmServiceCore::config() const noexcept {
  return config_;
}

void PdcmServiceCore::publish(const CoreState state,
                              const ProviderState provider_state,
                              const CoreDegradedReason degraded_reason,
                              const pdcm_status_t detail_status,
                              const std::uint32_t detected_device_count,
                              const std::uint64_t catalog_generation) {
  std::lock_guard<std::mutex> state_lock(state_mutex_);
  snapshot_.state = state;
  snapshot_.provider_state = provider_state;
  snapshot_.degraded_reason = degraded_reason;
  snapshot_.detail_status = detail_status;
  snapshot_.detected_device_count = detected_device_count;
  snapshot_.catalog_generation = catalog_generation;
}

} // namespace pdcm
