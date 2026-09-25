#include "core/runtime_config.hpp"

namespace pdcm {

Status RuntimeConfig::validate() const {
  if (target == TargetKind::kUnknown) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "runtime target must be explicit");
  }
  if (limits.max_sessions == 0 ||
      limits.max_outstanding_requests_per_session == 0 ||
      limits.max_watches_per_session == 0 ||
      limits.max_subscriptions_per_session == 0 ||
      limits.max_frame_bytes < 20) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "runtime resource limits must be positive and bounded");
  }
  if (provider_call_timeout.count() <= 0 || shutdown_grace.count() <= 0) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "runtime deadlines must be positive");
  }
  return Status::success();
}

} // namespace pdcm
