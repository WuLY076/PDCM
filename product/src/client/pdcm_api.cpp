#include "pdcm/pdcm.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#include "client/backend.hpp"
#include "client/embedded_backend.hpp"
#include "client/standalone_backend.hpp"
#include "common/clock.hpp"
#include "core/runtime_config.hpp"
#include "ipc/frame.hpp"
#include "provider/unavailable_provider.hpp"

namespace {

constexpr std::uint64_t kHandleMagic = UINT64_C(0x5044434D48444C31);
constexpr std::uint32_t kHandleType = 1;
constexpr std::uint32_t kHandleAbiVersion = 1;
constexpr std::uint32_t kKnownOpenFlags = PDCM_OPEN_ALLOW_EMBEDDED_FALLBACK;
constexpr const char *kDefaultEndpoint = "/run/pdcm/pdcm.sock";
constexpr std::size_t kMaxEndpointBytes = 107;

enum class HandleState : std::uint8_t {
  kOpen,
  kClosing,
  kClosed,
};

struct EffectiveOptions {
  pdcm_mode_t mode{PDCM_MODE_STANDALONE};
  std::uint32_t flags{0};
  std::string endpoint{kDefaultEndpoint};
  std::chrono::milliseconds deadline{1000};
  pdcm_target_t target{PDCM_TARGET_UNKNOWN};
};

} // namespace

struct pdcm_handle {
  std::uint64_t magic{kHandleMagic};
  std::uint32_t type{kHandleType};
  std::uint32_t abi_version{kHandleAbiVersion};
  std::atomic<HandleState> state{HandleState::kOpen};
  std::mutex mutex;
  std::condition_variable idle;
  std::size_t active_calls{0};
  std::unique_ptr<pdcm::ClientBackend> backend;
};

namespace {

pdcm_status_t validateOutputHeader(const pdcm_struct_header_t &header,
                                   const std::size_t required_size) {
  if (header.version != PDCM_STRUCT_VERSION_1 ||
      header.struct_size < required_size) {
    return PDCM_STATUS_INVALID_ARGUMENT;
  }
  return PDCM_STATUS_SUCCESS;
}

pdcm_status_t parseOptions(const pdcm_open_options_t *const options,
                           EffectiveOptions *const effective) {
  if (effective == nullptr) {
    return PDCM_STATUS_INTERNAL;
  }
  if (options == nullptr) {
    return PDCM_STATUS_SUCCESS;
  }
  if (validateOutputHeader(options->header, sizeof(pdcm_open_options_t)) !=
      PDCM_STATUS_SUCCESS) {
    return PDCM_STATUS_INVALID_ARGUMENT;
  }
  if ((options->flags & ~kKnownOpenFlags) != 0 || options->reserved != 0) {
    return PDCM_STATUS_INVALID_ARGUMENT;
  }
  if (options->required_protocol_major != pdcm::ipc::kProtocolMajor ||
      options->required_protocol_minor > pdcm::ipc::kProtocolMinor) {
    return PDCM_STATUS_PROTOCOL_INCOMPATIBLE;
  }

  switch (options->mode) {
  case PDCM_MODE_STANDALONE:
  case PDCM_MODE_EMBEDDED:
  case PDCM_MODE_AUTO:
    effective->mode = static_cast<pdcm_mode_t>(options->mode);
    break;
  default:
    return PDCM_STATUS_INVALID_ARGUMENT;
  }

  switch (options->target) {
  case PDCM_TARGET_UNKNOWN:
  case PDCM_TARGET_FPGA:
  case PDCM_TARGET_EMU:
    effective->target = static_cast<pdcm_target_t>(options->target);
    break;
  default:
    return PDCM_STATUS_INVALID_ARGUMENT;
  }

  effective->flags = options->flags;
  if (options->deadline_ns != 0) {
    if (options->deadline_ns >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return PDCM_STATUS_INVALID_ARGUMENT;
    }
    effective->deadline = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds(
            static_cast<std::int64_t>(options->deadline_ns)));
    if (effective->deadline.count() <= 0) {
      effective->deadline = std::chrono::milliseconds(1);
    }
  }

  if (effective->mode == PDCM_MODE_EMBEDDED) {
    if (options->endpoint != nullptr) {
      return PDCM_STATUS_INVALID_ARGUMENT;
    }
  } else if (options->endpoint != nullptr) {
    std::size_t length = 0;
    while (length <= kMaxEndpointBytes && options->endpoint[length] != '\0') {
      ++length;
    }
    if (length == 0 || length > kMaxEndpointBytes ||
        options->endpoint[0] != '/') {
      return PDCM_STATUS_INVALID_ARGUMENT;
    }
    effective->endpoint.assign(options->endpoint, length);
  }

  return PDCM_STATUS_SUCCESS;
}

pdcm::TargetKind toInternalTarget(const pdcm_target_t target) {
  switch (target) {
  case PDCM_TARGET_FPGA:
    return pdcm::TargetKind::kFpga;
  case PDCM_TARGET_EMU:
    return pdcm::TargetKind::kEmu;
  case PDCM_TARGET_UNKNOWN:
    return pdcm::TargetKind::kUnknown;
  }
  return pdcm::TargetKind::kUnknown;
}

std::uint32_t toPublicCoreState(const pdcm::CoreState state) {
  switch (state) {
  case pdcm::CoreState::kCreated:
    return PDCM_CORE_STATE_CREATED;
  case pdcm::CoreState::kStarting:
    return PDCM_CORE_STATE_STARTING;
  case pdcm::CoreState::kReady:
    return PDCM_CORE_STATE_READY;
  case pdcm::CoreState::kDegraded:
    return PDCM_CORE_STATE_DEGRADED;
  case pdcm::CoreState::kFailed:
    return PDCM_CORE_STATE_FAILED;
  case pdcm::CoreState::kStopping:
    return PDCM_CORE_STATE_STOPPING;
  case pdcm::CoreState::kStopped:
    return PDCM_CORE_STATE_STOPPED;
  }
  return PDCM_CORE_STATE_UNKNOWN;
}

std::uint32_t toPublicProviderState(const pdcm::ProviderState state) {
  switch (state) {
  case pdcm::ProviderState::kUninitialized:
    return PDCM_PROVIDER_STATE_UNINITIALIZED;
  case pdcm::ProviderState::kReady:
    return PDCM_PROVIDER_STATE_READY;
  case pdcm::ProviderState::kUnavailable:
    return PDCM_PROVIDER_STATE_UNAVAILABLE;
  case pdcm::ProviderState::kShutdown:
    return PDCM_PROVIDER_STATE_SHUTDOWN;
  }
  return PDCM_PROVIDER_STATE_UNKNOWN;
}

std::uint32_t toPublicFailurePhase(const pdcm::ProviderFailurePhase phase) {
  switch (phase) {
  case pdcm::ProviderFailurePhase::kNone:
    return PDCM_PROVIDER_FAILURE_NONE;
  case pdcm::ProviderFailurePhase::kLoad:
    return PDCM_PROVIDER_FAILURE_LOAD;
  case pdcm::ProviderFailurePhase::kEntry:
    return PDCM_PROVIDER_FAILURE_ENTRY;
  case pdcm::ProviderFailurePhase::kAbi:
    return PDCM_PROVIDER_FAILURE_ABI;
  case pdcm::ProviderFailurePhase::kNativeInitialize:
    return PDCM_PROVIDER_FAILURE_NATIVE_INITIALIZE;
  case pdcm::ProviderFailurePhase::kDiscovery:
    return PDCM_PROVIDER_FAILURE_DISCOVERY;
  }
  return PDCM_PROVIDER_FAILURE_NONE;
}

void copyVersionString(const std::string &source, char *const destination,
                       const std::size_t capacity) {
  if (capacity == 0) {
    return;
  }
  const std::size_t count = std::min(source.size(), capacity - 1);
  std::memcpy(destination, source.data(), count);
  destination[count] = '\0';
}

void fillLocalVersion(pdcm_version_info_t *const output) {
  *output = {};
  output->header.struct_size =
      static_cast<std::uint32_t>(sizeof(pdcm_version_info_t));
  output->header.version = PDCM_STRUCT_VERSION_1;
  copyVersionString("0.1.0", output->library_version,
                    sizeof(output->library_version));
  output->public_abi_major = PDCM_ABI_VERSION_MAJOR;
  output->public_abi_minor = PDCM_ABI_VERSION_MINOR;
  output->local_protocol_major = pdcm::ipc::kProtocolMajor;
  output->local_protocol_minor = pdcm::ipc::kProtocolMinor;
  output->core_state = PDCM_CORE_STATE_UNKNOWN;
  output->provider_state = PDCM_PROVIDER_STATE_UNKNOWN;
  output->provider_load_state = PDCM_PROVIDER_LOAD_NOT_ATTEMPTED;
  output->provider_compatibility = PDCM_PROVIDER_COMPATIBILITY_UNKNOWN;
  output->provider_failure_phase = PDCM_PROVIDER_FAILURE_NONE;
  output->detail_status = PDCM_STATUS_SUCCESS;
}
pdcm::EntityKind toInternalEntityKind(const std::uint32_t kind) {
  switch (kind) {
  case PDCM_ENTITY_KIND_DEVICE:
    return pdcm::EntityKind::kDevice;
  case PDCM_ENTITY_KIND_NODE:
    return pdcm::EntityKind::kNode;
  case PDCM_ENTITY_KIND_UNKNOWN:
    return pdcm::EntityKind::kUnknown;
  default:
    return pdcm::EntityKind::kUnknown;
  }
}

bool validPublicEntityKind(const std::uint32_t kind) {
  return kind == PDCM_ENTITY_KIND_UNKNOWN || kind == PDCM_ENTITY_KIND_DEVICE ||
         kind == PDCM_ENTITY_KIND_NODE;
}

pdcm_status_t toInternalEntityRef(const pdcm_entity_ref_t *const source,
                                  pdcm::EntityRef *const destination) {
  if (source == nullptr || destination == nullptr ||
      validateOutputHeader(source->header, sizeof(pdcm_entity_ref_t)) !=
          PDCM_STATUS_SUCCESS ||
      source->reserved != 0 || source->kind != PDCM_ENTITY_KIND_DEVICE ||
      source->generation == 0) {
    return PDCM_STATUS_INVALID_ARGUMENT;
  }
  destination->kind = pdcm::EntityKind::kDevice;
  destination->id = pdcm::EntityId{source->pdcm_id};
  destination->generation = source->generation;
  return PDCM_STATUS_SUCCESS;
}

std::uint32_t toPublicEntityState(const pdcm::EntityState state) {
  switch (state) {
  case pdcm::EntityState::kReady:
    return PDCM_ENTITY_STATE_READY;
  case pdcm::EntityState::kError:
    return PDCM_ENTITY_STATE_ERROR;
  case pdcm::EntityState::kUnknown:
    return PDCM_ENTITY_STATE_UNKNOWN;
  }
  return PDCM_ENTITY_STATE_UNKNOWN;
}

std::uint32_t toPublicObservationStatus(const pdcm::ObservationStatus status) {
  switch (status) {
  case pdcm::ObservationStatus::kValid:
    return PDCM_OBSERVATION_VALID;
  case pdcm::ObservationStatus::kStale:
    return PDCM_OBSERVATION_STALE;
  case pdcm::ObservationStatus::kNotAvailable:
    return PDCM_OBSERVATION_NOT_AVAILABLE;
  case pdcm::ObservationStatus::kUnsupported:
    return PDCM_OBSERVATION_UNSUPPORTED;
  case pdcm::ObservationStatus::kError:
    return PDCM_OBSERVATION_ERROR;
  }
  return PDCM_OBSERVATION_ERROR;
}

std::uint32_t toPublicCapabilityKind(const pdcm::CapabilityKind kind) {
  switch (kind) {
  case pdcm::CapabilityKind::kMetricsCatalog:
    return PDCM_CAPABILITY_METRICS_CATALOG;
  case pdcm::CapabilityKind::kMetric:
    return PDCM_CAPABILITY_METRIC;
  case pdcm::CapabilityKind::kHealth:
    return PDCM_CAPABILITY_HEALTH;
  }
  return 0;
}

std::uint32_t toPublicCapabilityReason(const pdcm::CapabilityReason reason) {
  switch (reason) {
  case pdcm::CapabilityReason::kSupported:
    return PDCM_CAPABILITY_REASON_SUPPORTED;
  case pdcm::CapabilityReason::kCatalogBlockedExternal:
    return PDCM_CAPABILITY_REASON_CATALOG_BLOCKED_EXTERNAL;
  case pdcm::CapabilityReason::kProviderUnsupported:
    return PDCM_CAPABILITY_REASON_PROVIDER_UNSUPPORTED;
  case pdcm::CapabilityReason::kDependencyMissing:
    return PDCM_CAPABILITY_REASON_DEPENDENCY_MISSING;
  case pdcm::CapabilityReason::kTemporarilyUnavailable:
    return PDCM_CAPABILITY_REASON_TEMPORARILY_UNAVAILABLE;
  case pdcm::CapabilityReason::kPermissionHidden:
    return PDCM_CAPABILITY_REASON_PERMISSION_HIDDEN;
  case pdcm::CapabilityReason::kPostP0Disabled:
    return PDCM_CAPABILITY_REASON_POST_P0_DISABLED;
  case pdcm::CapabilityReason::kTopologyUnsupported:
    return PDCM_CAPABILITY_REASON_TOPOLOGY_UNSUPPORTED;
  }
  return PDCM_CAPABILITY_REASON_PROVIDER_UNSUPPORTED;
}

void fillEntityRef(const pdcm::EntityRef &source,
                   pdcm_entity_ref_t *const destination) {
  *destination = {};
  destination->header.struct_size =
      static_cast<std::uint32_t>(sizeof(pdcm_entity_ref_t));
  destination->header.version = PDCM_STRUCT_VERSION_1;
  destination->kind = static_cast<std::uint32_t>(source.kind);
  destination->pdcm_id = source.id.value;
  destination->generation = source.generation;
}

void fillEntityInfo(const pdcm::EntityRecord &source,
                    pdcm_entity_info_t *const destination) {
  *destination = {};
  destination->header.struct_size =
      static_cast<std::uint32_t>(sizeof(pdcm_entity_info_t));
  destination->header.version = PDCM_STRUCT_VERSION_1;
  fillEntityRef(source.ref, &destination->entity);
  destination->state = toPublicEntityState(source.state);
  destination->item_status = static_cast<std::int32_t>(source.item_status);
  destination->native_id_status =
      toPublicObservationStatus(source.native_id.status);
  destination->pci_bdf_status =
      toPublicObservationStatus(source.pci_bdf.status);
  destination->pdrv_version_status =
      toPublicObservationStatus(source.pdrv_version.status);
  copyVersionString(source.native_id.value, destination->native_id,
                    sizeof(destination->native_id));
  copyVersionString(source.pci_bdf.value, destination->pci_bdf,
                    sizeof(destination->pci_bdf));
  copyVersionString(source.pdrv_version.value, destination->pdrv_version,
                    sizeof(destination->pdrv_version));
}

void fillCapabilityItem(const pdcm::CapabilityItem &source,
                        pdcm_capability_item_t *const destination) {
  *destination = {};
  destination->header.struct_size =
      static_cast<std::uint32_t>(sizeof(pdcm_capability_item_t));
  destination->header.version = PDCM_STRUCT_VERSION_1;
  destination->kind = toPublicCapabilityKind(source.kind);
  destination->id = source.id;
  destination->supported = source.supported ? 1U : 0U;
  destination->reason = toPublicCapabilityReason(source.reason);
  destination->semantic_version = source.semantic_version;
  destination->catalog_generation = source.catalog_generation;
}

std::uint32_t toPublicHealthState(const pdcm::HealthState state) {
  switch (state) {
  case pdcm::HealthState::kHealthy:
    return PDCM_HEALTH_STATE_HEALTHY;
  case pdcm::HealthState::kUnknown:
    return PDCM_HEALTH_STATE_UNKNOWN;
  case pdcm::HealthState::kWarning:
    return PDCM_HEALTH_STATE_WARNING;
  case pdcm::HealthState::kError:
    return PDCM_HEALTH_STATE_ERROR;
  }
  return PDCM_HEALTH_STATE_UNKNOWN;
}

void fillHealthResult(const pdcm::HealthResult &source,
                      pdcm_health_result_t *const destination) {
  *destination = {};
  destination->header.struct_size =
      static_cast<std::uint32_t>(sizeof(pdcm_health_result_t));
  destination->header.version = PDCM_STRUCT_VERSION_1;
  fillEntityRef(source.entity, &destination->entity);
  destination->subsystem_id = source.subsystem_id;
  destination->state = toPublicHealthState(source.state);
  destination->item_status = toPublicObservationStatus(source.item_status);
  destination->code = static_cast<std::uint32_t>(source.code);
  destination->catalog_generation = source.catalog_generation;
  destination->evaluated_monotonic_time_ns =
      source.evaluated_monotonic_time_ns;
  destination->evidence_age_ns = source.evidence_age_ns;
  if (!source.evidence.empty()) {
    destination->evidence_id = source.evidence.front().evidence_id;
    destination->sequence_or_token = source.evidence.front().sequence_or_token;
    copyVersionString(source.evidence.front().source.provider,
                      destination->source, sizeof(destination->source));
  }
  if (!source.limitations.empty()) {
    copyVersionString(source.limitations.front().detail,
                      destination->limitation,
                      sizeof(destination->limitation));
  }
}

class ActiveCall {
public:
  ActiveCall() = default;
  ~ActiveCall() { release(); }

  ActiveCall(const ActiveCall &) = delete;
  ActiveCall &operator=(const ActiveCall &) = delete;

  pdcm_status_t acquire(pdcm_handle *const handle) {
    if (handle == nullptr) {
      return PDCM_STATUS_INVALID_ARGUMENT;
    }

    std::lock_guard<std::mutex> lock(handle->mutex);
    if (handle->magic != kHandleMagic || handle->type != kHandleType ||
        handle->abi_version != kHandleAbiVersion ||
        handle->state.load() != HandleState::kOpen ||
        handle->backend == nullptr) {
      return PDCM_STATUS_INVALID_ARGUMENT;
    }

    ++handle->active_calls;
    handle_ = handle;
    backend_ = handle->backend.get();
    return PDCM_STATUS_SUCCESS;
  }

  [[nodiscard]] pdcm::ClientBackend *backend() const noexcept {
    return backend_;
  }

private:
  void release() noexcept {
    if (handle_ == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(handle_->mutex);
    --handle_->active_calls;
    if (handle_->active_calls == 0) {
      handle_->idle.notify_all();
    }
    handle_ = nullptr;
    backend_ = nullptr;
  }

  pdcm_handle *handle_{nullptr};
  pdcm::ClientBackend *backend_{nullptr};
};

pdcm_status_t openEmbedded(const EffectiveOptions &options,
                           pdcm_handle_t **const out_handle) {
  if (options.target == PDCM_TARGET_UNKNOWN) {
    return PDCM_STATUS_INVALID_ARGUMENT;
  }

  pdcm::RuntimeConfig config;
  config.target = toInternalTarget(options.target);
  config.provider_call_timeout = options.deadline;

  auto backend = std::make_unique<pdcm::EmbeddedBackend>(
      config, std::make_unique<pdcm::UnavailableProvider>(),
      std::make_shared<pdcm::SystemClock>());
  const pdcm::Status started = backend->start();
  if (!started.ok()) {
    return started.code();
  }

  auto handle = std::make_unique<pdcm_handle>();
  handle->backend = std::move(backend);
  *out_handle = handle.release();
  return PDCM_STATUS_SUCCESS;
}

pdcm_status_t openStandalone(const EffectiveOptions &options,
                             pdcm_handle_t **const out_handle) {
  auto backend = std::make_unique<pdcm::StandaloneBackend>(
      options.endpoint, options.deadline,
      pdcm::ResourceLimits{}.max_frame_bytes);
  const pdcm::Status started = backend->start();
  if (!started.ok()) {
    return started.code();
  }

  auto handle = std::make_unique<pdcm_handle>();
  handle->backend = std::move(backend);
  *out_handle = handle.release();
  return PDCM_STATUS_SUCCESS;
}

pdcm_status_t openSelectedBackend(const EffectiveOptions &options,
                                  pdcm_handle_t **const out_handle) {
  if (options.mode == PDCM_MODE_EMBEDDED) {
    return openEmbedded(options, out_handle);
  }

  const pdcm_status_t standalone_status = openStandalone(options, out_handle);
  if (standalone_status == PDCM_STATUS_SUCCESS ||
      options.mode == PDCM_MODE_STANDALONE ||
      (options.flags & PDCM_OPEN_ALLOW_EMBEDDED_FALLBACK) == 0 ||
      (standalone_status != PDCM_STATUS_UNAVAILABLE &&
       standalone_status != PDCM_STATUS_TIMEOUT)) {
    return standalone_status;
  }
  return openEmbedded(options, out_handle);
}

} // namespace

extern "C" PDCM_API pdcm_status_t
pdcm_open(const pdcm_open_options_t *const options,
          pdcm_handle_t **const out_handle) {
  try {
    if (out_handle == nullptr || *out_handle != nullptr) {
      return PDCM_STATUS_INVALID_ARGUMENT;
    }

    EffectiveOptions effective;
    const pdcm_status_t options_status = parseOptions(options, &effective);
    if (options_status != PDCM_STATUS_SUCCESS) {
      return options_status;
    }
    return openSelectedBackend(effective, out_handle);
  } catch (const std::bad_alloc &) {
    return PDCM_STATUS_RESOURCE_EXHAUSTED;
  } catch (...) {
    return PDCM_STATUS_INTERNAL;
  }
}

extern "C" PDCM_API pdcm_status_t pdcm_version_get(
    pdcm_handle_t *const handle, pdcm_version_info_t *const out_version) {
  try {
    if (out_version == nullptr ||
        validateOutputHeader(out_version->header,
                             sizeof(pdcm_version_info_t)) !=
            PDCM_STATUS_SUCCESS) {
      return PDCM_STATUS_INVALID_ARGUMENT;
    }

    ActiveCall call;
    if (handle != nullptr) {
      const pdcm_status_t acquire_status = call.acquire(handle);
      if (acquire_status != PDCM_STATUS_SUCCESS) {
        return acquire_status;
      }
    }

    fillLocalVersion(out_version);
    if (handle == nullptr) {
      return PDCM_STATUS_SUCCESS;
    }

    pdcm::BackendVersion backend_version;
    const pdcm::Status status = call.backend()->version(&backend_version);
    if (!status.ok()) {
      return status.code();
    }

    copyVersionString(backend_version.daemon_version,
                      out_version->daemon_version,
                      sizeof(out_version->daemon_version));
    out_version->negotiated_protocol_major = backend_version.protocol_major;
    out_version->negotiated_protocol_minor = backend_version.protocol_minor;
    out_version->mode = backend_version.mode == pdcm::BackendMode::kEmbedded
                            ? PDCM_MODE_EMBEDDED
                            : PDCM_MODE_STANDALONE;
    out_version->target = static_cast<std::uint32_t>(backend_version.target);
    out_version->core_state = toPublicCoreState(backend_version.core.state);
    out_version->provider_state =
        toPublicProviderState(backend_version.core.provider_state);
    out_version->provider_load_state =
        backend_version.provider_loaded
            ? PDCM_PROVIDER_LOAD_LOADED
            : (backend_version.provider_load_attempted
                   ? PDCM_PROVIDER_LOAD_FAILED
                   : PDCM_PROVIDER_LOAD_NOT_ATTEMPTED);
    out_version->provider_compatibility =
        backend_version.provider_abi_compatible
            ? PDCM_PROVIDER_COMPATIBILITY_COMPATIBLE
            : PDCM_PROVIDER_COMPATIBILITY_UNKNOWN;
    out_version->provider_failure_phase =
        toPublicFailurePhase(backend_version.provider_failure_phase);
    out_version->detail_status =
        static_cast<std::int32_t>(backend_version.core.detail_status);
    out_version->detected_device_count =
        backend_version.core.detected_device_count;
    out_version->session_id = backend_version.session_id;
    out_version->catalog_generation = backend_version.core.catalog_generation;
    return PDCM_STATUS_SUCCESS;
  } catch (const std::bad_alloc &) {
    return PDCM_STATUS_RESOURCE_EXHAUSTED;
  } catch (...) {
    return PDCM_STATUS_INTERNAL;
  }
}

extern "C" PDCM_API pdcm_status_t pdcm_entity_list(
    pdcm_handle_t *const handle, const pdcm_entity_filter_t *const filter,
    pdcm_entity_info_t *const entities, std::size_t *const inout_count) {
  try {
    if (inout_count == nullptr) {
      return PDCM_STATUS_INVALID_ARGUMENT;
    }

    pdcm::EntityKind kind = pdcm::EntityKind::kUnknown;
    if (filter != nullptr) {
      if (validateOutputHeader(filter->header, sizeof(pdcm_entity_filter_t)) !=
              PDCM_STATUS_SUCCESS ||
          filter->reserved != 0 || !validPublicEntityKind(filter->kind)) {
        return PDCM_STATUS_INVALID_ARGUMENT;
      }
      kind = toInternalEntityKind(filter->kind);
    }

    ActiveCall call;
    const pdcm_status_t acquired = call.acquire(handle);
    if (acquired != PDCM_STATUS_SUCCESS) {
      return acquired;
    }

    const std::size_t capacity = *inout_count;
    const pdcm::EntityListResult result = call.backend()->entities(kind);
    const pdcm_status_t status = result.status.code();
    if (status == PDCM_STATUS_UNSUPPORTED) {
      *inout_count = result.detected_device_count;
      return status;
    }
    if (status != PDCM_STATUS_SUCCESS && status != PDCM_STATUS_PARTIAL_RESULT) {
      *inout_count = 0;
      return status;
    }

    *inout_count = result.entities.size();
    if (entities == nullptr) {
      return status;
    }
    if (capacity < result.entities.size()) {
      return PDCM_STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t index = 0; index < result.entities.size(); ++index) {
      if (validateOutputHeader(entities[index].header,
                               sizeof(pdcm_entity_info_t)) !=
              PDCM_STATUS_SUCCESS ||
          entities[index].reserved != 0) {
        return PDCM_STATUS_INVALID_ARGUMENT;
      }
    }
    for (std::size_t index = 0; index < result.entities.size(); ++index) {
      fillEntityInfo(result.entities[index], &entities[index]);
    }
    return status;
  } catch (const std::bad_alloc &) {
    return PDCM_STATUS_RESOURCE_EXHAUSTED;
  } catch (...) {
    return PDCM_STATUS_INTERNAL;
  }
}

extern "C" PDCM_API pdcm_status_t pdcm_capability_query(
    pdcm_handle_t *const handle, const pdcm_entity_ref_t *const entity,
    pdcm_capability_set_t *const out_capabilities) {
  try {
    if (out_capabilities == nullptr ||
        validateOutputHeader(out_capabilities->header,
                             sizeof(pdcm_capability_set_t)) !=
            PDCM_STATUS_SUCCESS ||
        (out_capabilities->items == nullptr &&
         out_capabilities->item_capacity != 0)) {
      return PDCM_STATUS_INVALID_ARGUMENT;
    }

    pdcm::EntityRef internal_entity;
    const pdcm_status_t entity_status =
        toInternalEntityRef(entity, &internal_entity);
    if (entity_status != PDCM_STATUS_SUCCESS) {
      return entity_status;
    }

    ActiveCall call;
    const pdcm_status_t acquired = call.acquire(handle);
    if (acquired != PDCM_STATUS_SUCCESS) {
      return acquired;
    }

    const pdcm::CapabilityQueryResult result =
        call.backend()->capabilities(internal_entity);
    if (!result.status.ok() || !result.capabilities.has_value()) {
      out_capabilities->item_count = 0;
      return result.status.code();
    }

    const pdcm::CapabilitySet &source = *result.capabilities;
    fillEntityRef(source.entity, &out_capabilities->entity);
    out_capabilities->catalog_generation = source.catalog_generation;
    out_capabilities->item_count = source.items.size();
    if (source.items.size() > out_capabilities->item_capacity ||
        (source.items.size() != 0 && out_capabilities->items == nullptr)) {
      return PDCM_STATUS_BUFFER_TOO_SMALL;
    }

    for (std::size_t index = 0; index < source.items.size(); ++index) {
      if (validateOutputHeader(out_capabilities->items[index].header,
                               sizeof(pdcm_capability_item_t)) !=
              PDCM_STATUS_SUCCESS ||
          out_capabilities->items[index].reserved != 0) {
        return PDCM_STATUS_INVALID_ARGUMENT;
      }
    }
    for (std::size_t index = 0; index < source.items.size(); ++index) {
      fillCapabilityItem(source.items[index], &out_capabilities->items[index]);
    }
    return PDCM_STATUS_SUCCESS;
  } catch (const std::bad_alloc &) {
    return PDCM_STATUS_RESOURCE_EXHAUSTED;
  } catch (...) {
    return PDCM_STATUS_INTERNAL;
  }
}

extern "C" PDCM_API pdcm_status_t
pdcm_health_query(pdcm_handle_t *const handle,
                  const pdcm_health_request_t *const request,
                  pdcm_health_result_t *const out_result) {
  try {
    if (request == nullptr || out_result == nullptr ||
        validateOutputHeader(request->header,
                             sizeof(pdcm_health_request_t)) !=
            PDCM_STATUS_SUCCESS ||
        validateOutputHeader(out_result->header,
                             sizeof(pdcm_health_result_t)) !=
            PDCM_STATUS_SUCCESS ||
        request->reserved != 0 || request->subsystem_id == 0) {
      return PDCM_STATUS_INVALID_ARGUMENT;
    }

    pdcm::EntityRef entity;
    const pdcm_status_t entity_status =
        toInternalEntityRef(&request->entity, &entity);
    if (entity_status != PDCM_STATUS_SUCCESS) {
      return entity_status;
    }

    ActiveCall call;
    const pdcm_status_t acquired = call.acquire(handle);
    if (acquired != PDCM_STATUS_SUCCESS) {
      return acquired;
    }

    const pdcm::HealthQueryResult result = call.backend()->health(
        {entity, request->subsystem_id, request->catalog_generation,
         request->max_age_ns});
    if (result.status.code() != PDCM_STATUS_SUCCESS &&
        result.status.code() != PDCM_STATUS_PARTIAL_RESULT) {
      return result.status.code();
    }
    if (!result.item.has_value()) {
      return PDCM_STATUS_INTERNAL;
    }
    fillHealthResult(*result.item, out_result);
    return result.status.code();
  } catch (const std::bad_alloc &) {
    return PDCM_STATUS_RESOURCE_EXHAUSTED;
  } catch (...) {
    return PDCM_STATUS_INTERNAL;
  }
}

extern "C" PDCM_API pdcm_status_t
pdcm_close(pdcm_handle_t **const handle_pointer) {
  try {
    if (handle_pointer == nullptr || *handle_pointer == nullptr) {
      return PDCM_STATUS_INVALID_ARGUMENT;
    }

    pdcm_handle *const handle = *handle_pointer;
    std::unique_ptr<pdcm::ClientBackend> backend;
    {
      std::unique_lock<std::mutex> lock(handle->mutex);
      if (handle->magic != kHandleMagic || handle->type != kHandleType ||
          handle->abi_version != kHandleAbiVersion ||
          handle->state.load() != HandleState::kOpen ||
          handle->backend == nullptr) {
        return PDCM_STATUS_INVALID_ARGUMENT;
      }

      handle->state.store(HandleState::kClosing);
      handle->idle.wait(lock, [handle] { return handle->active_calls == 0; });
      backend = std::move(handle->backend);
      handle->magic = 0;
      handle->state.store(HandleState::kClosed);
    }

    const pdcm::Status status = backend->close();
    delete handle;
    *handle_pointer = nullptr;
    return status.code();
  } catch (const std::bad_alloc &) {
    return PDCM_STATUS_RESOURCE_EXHAUSTED;
  } catch (...) {
    return PDCM_STATUS_INTERNAL;
  }
}
