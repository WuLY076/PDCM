#ifndef PDCM_CORE_RUNTIME_CONFIG_HPP_
#define PDCM_CORE_RUNTIME_CONFIG_HPP_

#include <chrono>
#include <cstddef>

#include "common/status.hpp"
#include "provider/provider.hpp"

namespace pdcm {

struct ResourceLimits {
  std::size_t max_sessions{64};
  std::size_t max_outstanding_requests_per_session{32};
  std::size_t max_watches_per_session{64};
  std::size_t max_subscriptions_per_session{32};
  std::size_t max_frame_bytes{4U * 1024U * 1024U};
};

struct RuntimeConfig {
  TargetKind target{TargetKind::kUnknown};
  ResourceLimits limits{};
  std::chrono::milliseconds provider_call_timeout{1000};
  std::chrono::milliseconds shutdown_grace{5000};

  [[nodiscard]] Status validate() const;
};

} // namespace pdcm

#endif // PDCM_CORE_RUNTIME_CONFIG_HPP_
