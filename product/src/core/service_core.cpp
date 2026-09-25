#include "core/service_core.hpp"

#include <stdexcept>
#include <utility>

namespace pdcm {
namespace {

TargetCatalog defaultCatalog(const TargetKind target) {
  return TargetCatalog::blocked(
      target == TargetKind::kUnknown ? TargetKind::kFpga : target);
}

std::shared_ptr<const Clock>
requireClock(std::shared_ptr<const Clock> clock) {
  if (!clock) {
    throw std::invalid_argument("PdcmServiceCore requires a clock");
  }
  return clock;
}

} // namespace

PdcmServiceCore::PdcmServiceCore(RuntimeConfig config,
                                 std::unique_ptr<Provider> provider,
                                 std::shared_ptr<const Clock> clock)
    : PdcmServiceCore(config, std::move(provider), std::move(clock),
                      defaultCatalog(config.target)) {}

PdcmServiceCore::PdcmServiceCore(RuntimeConfig config,
                                 std::unique_ptr<Provider> provider,
                                 std::shared_ptr<const Clock> clock,
                                 TargetCatalog target_catalog)
    : config_(std::move(config)), provider_manager_(std::move(provider)),
      clock_(requireClock(std::move(clock))), target_catalog_(target_catalog),
      semantic_catalog_(std::move(target_catalog)),
      metrics_manager_(data_manager_, *clock_) {}

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

  const CatalogCommitResult committed =
      semantic_catalog_.commit(discovery.descriptor);
  if (!committed.committed) {
    publish(CoreState::kDegraded, provider_manager_.state(),
            CoreDegradedReason::kDiscoveryFailed, committed.status.code(),
            discovery.descriptor.detected_device_count,
            committed.catalog_generation);
    return Status::success();
  }

  if (committed.status.code() == PDCM_STATUS_UNSUPPORTED) {
    publish(CoreState::kDegraded, provider_manager_.state(),
            CoreDegradedReason::kTopologyUnsupported, PDCM_STATUS_UNSUPPORTED,
            committed.detected_device_count, committed.catalog_generation);
    return Status::success();
  }

  const std::shared_ptr<const CatalogView> catalog =
      semantic_catalog_.snapshot();
  const Status data_status =
      data_manager_.activateCatalog(catalog, target_catalog_);
  const Status metrics_status =
      metrics_manager_.activateCatalog(catalog, target_catalog_);
  if (!data_status.ok() || !metrics_status.ok()) {
    const pdcm_status_t failure =
        !data_status.ok() ? data_status.code() : metrics_status.code();
    publish(CoreState::kFailed, provider_manager_.state(),
            CoreDegradedReason::kDiscoveryFailed, failure,
            committed.detected_device_count, committed.catalog_generation);
    return Status(failure, "core data services activation failed");
  }
  (void)metrics_manager_.onProviderStateChanged(provider_manager_.state());

  publish(CoreState::kReady, provider_manager_.state(),
          CoreDegradedReason::kNone, committed.status.code(),
          committed.detected_device_count, committed.catalog_generation);
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
std::shared_ptr<const CatalogView>
PdcmServiceCore::catalogSnapshot() const noexcept {
  return semantic_catalog_.snapshot();
}
EntityListResult PdcmServiceCore::entities(const EntityKind kind) const {
  const CoreSnapshot core = snapshot();
  const std::shared_ptr<const CatalogView> view = catalogSnapshot();
  if (core.state == CoreState::kCreated || core.state == CoreState::kStarting ||
      core.state == CoreState::kFailed || core.state == CoreState::kStopping ||
      core.state == CoreState::kStopped) {
    EntityListResult result;
    result.status =
        Status(PDCM_STATUS_NOT_INITIALIZED, "core is not serving discovery");
    return result;
  }
  if (view->generation() == 0) {
    EntityListResult result;
    result.status =
        Status(PDCM_STATUS_UNAVAILABLE, "no discovery snapshot is available");
    result.detected_device_count = core.detected_device_count;
    return result;
  }
  return view->list(kind);
}

CapabilityQueryResult
PdcmServiceCore::capabilities(const EntityRef entity) const {
  const CoreSnapshot core = snapshot();
  const std::shared_ptr<const CatalogView> view = catalogSnapshot();
  if (core.state == CoreState::kCreated || core.state == CoreState::kStarting ||
      core.state == CoreState::kFailed || core.state == CoreState::kStopping ||
      core.state == CoreState::kStopped) {
    return {
        Status(PDCM_STATUS_NOT_INITIALIZED, "core is not serving capabilities"),
        std::nullopt};
  }
  if (view->generation() == 0) {
    return {
        Status(PDCM_STATUS_UNAVAILABLE, "no capability snapshot is available"),
        std::nullopt};
  }
  return view->capabilities(entity);
}

HealthQueryResult
PdcmServiceCore::health(const HealthRequest &request) const {
  const CoreSnapshot core = snapshot();
  if (core.state == CoreState::kCreated || core.state == CoreState::kStarting ||
      core.state == CoreState::kFailed || core.state == CoreState::kStopping ||
      core.state == CoreState::kStopped) {
    return {Status(PDCM_STATUS_NOT_INITIALIZED,
                   "core is not serving health"),
            std::nullopt, std::nullopt};
  }
  if (core.catalog_generation == 0) {
    return {Status(PDCM_STATUS_UNAVAILABLE,
                   "health catalog is unavailable"),
            std::nullopt, std::nullopt};
  }
  return metrics_manager_.queryHealth(request);
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
