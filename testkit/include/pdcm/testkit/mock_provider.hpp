#ifndef PDCM_TESTKIT_MOCK_PROVIDER_HPP_
#define PDCM_TESTKIT_MOCK_PROVIDER_HPP_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include "common/clock.hpp"
#include "provider/provider.hpp"

namespace pdcm::testkit {

enum class MockScenario : std::uint8_t {
  kProviderUnavailable,
  kZeroDevice,
  kSingleDevice,
  kMultipleDevices,
  kPartialMetricFailure,
};

enum class MockHeartbeatClass : std::uint64_t {
  kNormal = 0,
  kWarning = 1,
  kFault = 2,
};

struct MockProviderConfig {
  MockScenario scenario{MockScenario::kSingleDevice};
  MockHeartbeatClass heartbeat{MockHeartbeatClass::kNormal};
  TargetKind target{TargetKind::kFpga};
};

class MockProvider final : public Provider {
public:
  MockProvider(MockProviderConfig config, std::shared_ptr<const Clock> clock);

  Status initialize(const ProviderInitOptions &options) override;
  void shutdown() noexcept override;

  [[nodiscard]] ProviderState state() const noexcept override;
  [[nodiscard]] ProviderConcurrency concurrency() const noexcept override;

  ProviderDiscoveryResult discover(MonotonicTime deadline) override;
  ProviderReadResult batchRead(const ProviderReadRequest &request) override;

private:
  [[nodiscard]] ProviderDescriptor descriptor() const;
  [[nodiscard]] ProviderReadItemResult readItem(const ProviderReadItem &item,
                                                std::uint64_t sequence) const;

  MockProviderConfig config_;
  std::shared_ptr<const Clock> clock_;
  std::atomic<ProviderState> state_{ProviderState::kUninitialized};
  mutable std::mutex read_mutex_;
  std::uint64_t sample_sequence_{0};
};

} // namespace pdcm::testkit

#endif // PDCM_TESTKIT_MOCK_PROVIDER_HPP_
