#include "provider/provider_manager.hpp"

#include <stdexcept>
#include <utility>

namespace pdcm {

ProviderManager::ProviderManager(std::unique_ptr<Provider> provider)
    : provider_(std::move(provider)) {
  if (!provider_) {
    throw std::invalid_argument("ProviderManager requires a provider");
  }
}

ProviderManager::~ProviderManager() { shutdown(); }

Status ProviderManager::initialize(const ProviderInitOptions &options) {
  std::lock_guard<std::mutex> call_lock(call_mutex_);
  if (state_.load() == ProviderState::kShutdown) {
    return Status(PDCM_STATUS_NOT_INITIALIZED,
                  "provider manager is already shut down");
  }

  const Status status = provider_->initialize(options);
  state_.store(provider_->state());
  if (status.ok() && state_.load() != ProviderState::kReady) {
    state_.store(ProviderState::kUnavailable);
    return Status(PDCM_STATUS_INTERNAL,
                  "provider reported success without entering ready state");
  }
  return status;
}

ProviderDiscoveryResult
ProviderManager::discover(const MonotonicTime deadline) {
  std::lock_guard<std::mutex> call_lock(call_mutex_);
  if (state_.load() != ProviderState::kReady) {
    ProviderDiscoveryResult result;
    result.call_status =
        Status(PDCM_STATUS_UNAVAILABLE, "provider manager is not ready");
    result.descriptor.state = state_.load();
    return result;
  }

  ProviderDiscoveryResult result = provider_->discover(deadline);
  state_.store(provider_->state());
  return result;
}

ProviderReadResult
ProviderManager::batchRead(const ProviderReadRequest &request) {
  std::lock_guard<std::mutex> call_lock(call_mutex_);
  if (state_.load() != ProviderState::kReady) {
    ProviderReadResult result;
    result.call_status =
        Status(PDCM_STATUS_UNAVAILABLE, "provider manager is not ready");
    return result;
  }

  ProviderReadResult result = provider_->batchRead(request);
  state_.store(provider_->state());
  return result;
}

void ProviderManager::shutdown() noexcept {
  std::lock_guard<std::mutex> call_lock(call_mutex_);
  if (state_.load() == ProviderState::kShutdown) {
    return;
  }
  provider_->shutdown();
  state_.store(ProviderState::kShutdown);
}

ProviderState ProviderManager::state() const noexcept { return state_.load(); }

} // namespace pdcm
