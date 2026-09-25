#include "client/embedded_backend.hpp"

#include <utility>

#include "ipc/frame.hpp"

namespace pdcm {

EmbeddedBackend::EmbeddedBackend(RuntimeConfig config,
                                 std::unique_ptr<Provider> provider,
                                 std::shared_ptr<const Clock> clock)
    : config_(config), core_(config, std::move(provider), std::move(clock)),
      sessions_(config.limits) {}

EmbeddedBackend::~EmbeddedBackend() { (void)close(); }

Status EmbeddedBackend::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) {
    return Status(PDCM_STATUS_NOT_INITIALIZED,
                  "embedded backend is already closed");
  }
  if (started_) {
    return Status(PDCM_STATUS_NOT_INITIALIZED,
                  "embedded backend is already started");
  }

  Status core_status = core_.start();
  if (!core_status.ok()) {
    return core_status;
  }

  const SessionCreateResult created = sessions_.beginHandshake(PeerIdentity{});
  if (!created.status.ok()) {
    (void)core_.stop();
    return created.status;
  }

  Status active = sessions_.activate(created.id);
  if (!active.ok()) {
    (void)sessions_.close(created.id);
    (void)core_.stop();
    return active;
  }

  session_id_ = created.id;
  started_ = true;
  return Status::success();
}

Status EmbeddedBackend::version(BackendVersion *const version) const {
  if (version == nullptr) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "version output must not be null");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || closed_) {
    return Status(PDCM_STATUS_NOT_INITIALIZED,
                  "embedded backend is not active");
  }

  version->daemon_version = "embedded";
  version->mode = BackendMode::kEmbedded;
  version->target = config_.target;
  version->core = core_.snapshot();
  version->provider_failure_phase =
      version->core.degraded_reason == CoreDegradedReason::kDiscoveryFailed
          ? ProviderFailurePhase::kDiscovery
          : ProviderFailurePhase::kNone;
  version->provider_load_attempted = false;
  version->provider_loaded =
      version->core.provider_state == ProviderState::kReady;
  version->provider_abi_compatible = version->provider_loaded;
  version->protocol_major = ipc::kProtocolMajor;
  version->protocol_minor = ipc::kProtocolMinor;
  version->session_id = session_id_;
  return Status::success();
}
EntityListResult EmbeddedBackend::entities(const EntityKind kind) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || closed_) {
    EntityListResult result;
    result.status =
        Status(PDCM_STATUS_NOT_INITIALIZED, "embedded backend is not active");
    return result;
  }
  return core_.entities(kind);
}

CapabilityQueryResult
EmbeddedBackend::capabilities(const EntityRef entity) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || closed_) {
    return {
        Status(PDCM_STATUS_NOT_INITIALIZED, "embedded backend is not active"),
        std::nullopt};
  }
  return core_.capabilities(entity);
}

HealthQueryResult
EmbeddedBackend::health(const HealthRequest &request) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || closed_) {
    return {
        Status(PDCM_STATUS_NOT_INITIALIZED, "embedded backend is not active"),
        std::nullopt, std::nullopt};
  }
  return core_.health(request);
}

Status EmbeddedBackend::close() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) {
    return Status::success();
  }

  (void)sessions_.beginDraining();
  if (session_id_ != 0) {
    (void)sessions_.close(session_id_);
    session_id_ = 0;
  }
  sessions_.closeAll();
  const Status core_status = core_.stop();
  closed_ = true;
  started_ = false;
  return core_status;
}

} // namespace pdcm
