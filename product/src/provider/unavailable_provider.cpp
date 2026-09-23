#include "provider/unavailable_provider.hpp"

namespace pdcm {

Status UnavailableProvider::initialize(const ProviderInitOptions &) {
  state_.store(ProviderState::kUnavailable);
  return Status(PDCM_STATUS_UNAVAILABLE,
                "production provider integration is not configured");
}

void UnavailableProvider::shutdown() noexcept {
  state_.store(ProviderState::kShutdown);
}

ProviderState UnavailableProvider::state() const noexcept {
  return state_.load();
}

ProviderConcurrency UnavailableProvider::concurrency() const noexcept {
  ProviderConcurrency concurrency;
  concurrency.max_concurrency = 1;
  concurrency.reentrant = false;
  return concurrency;
}

ProviderDiscoveryResult UnavailableProvider::discover(const MonotonicTime) {
  ProviderDiscoveryResult result;
  result.call_status =
      Status(PDCM_STATUS_UNAVAILABLE, "provider is unavailable");
  result.descriptor.state = state_.load();
  result.descriptor.failure_phase = ProviderFailurePhase::kLoad;
  return result;
}

ProviderReadResult UnavailableProvider::batchRead(const ProviderReadRequest &) {
  ProviderReadResult result;
  result.call_status =
      Status(PDCM_STATUS_UNAVAILABLE, "provider is unavailable");
  return result;
}

} // namespace pdcm
