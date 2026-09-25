#ifndef PDCM_PROVIDER_UNAVAILABLE_PROVIDER_HPP_
#define PDCM_PROVIDER_UNAVAILABLE_PROVIDER_HPP_

#include <atomic>

#include "provider/provider.hpp"

namespace pdcm {

class UnavailableProvider final : public Provider {
public:
  Status initialize(const ProviderInitOptions &options) override;
  void shutdown() noexcept override;

  [[nodiscard]] ProviderState state() const noexcept override;
  [[nodiscard]] ProviderConcurrency concurrency() const noexcept override;

  ProviderDiscoveryResult discover(MonotonicTime deadline) override;
  ProviderReadResult batchRead(const ProviderReadRequest &request) override;

private:
  std::atomic<ProviderState> state_{ProviderState::kUninitialized};
};

} // namespace pdcm

#endif // PDCM_PROVIDER_UNAVAILABLE_PROVIDER_HPP_
