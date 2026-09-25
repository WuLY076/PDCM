#ifndef PDCM_PROVIDER_PROVIDER_MANAGER_HPP_
#define PDCM_PROVIDER_PROVIDER_MANAGER_HPP_

#include <atomic>
#include <memory>
#include <mutex>

#include "provider/provider.hpp"

namespace pdcm {

class ProviderManager {
public:
  explicit ProviderManager(std::unique_ptr<Provider> provider);
  ~ProviderManager();

  ProviderManager(const ProviderManager &) = delete;
  ProviderManager &operator=(const ProviderManager &) = delete;

  Status initialize(const ProviderInitOptions &options);
  ProviderDiscoveryResult discover(MonotonicTime deadline);
  ProviderReadResult batchRead(const ProviderReadRequest &request);
  void shutdown() noexcept;

  [[nodiscard]] ProviderState state() const noexcept;

private:
  std::unique_ptr<Provider> provider_;
  std::atomic<ProviderState> state_{ProviderState::kUninitialized};
  std::mutex call_mutex_;
};

} // namespace pdcm

#endif // PDCM_PROVIDER_PROVIDER_MANAGER_HPP_
