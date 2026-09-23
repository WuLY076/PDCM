#include "pdcm/pdcm.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#include "client/backend.hpp"
#include "client/embedded_backend.hpp"
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
  if (options->required_protocol_major != pdcm::ipc::kProtocolMajor) {
    return PDCM_STATUS_UNSUPPORTED;
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
    effective->deadline = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds(options->deadline_ns));
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

pdcm_status_t openSelectedBackend(const EffectiveOptions &options,
                                  pdcm_handle_t **const out_handle) {
  if (options.mode == PDCM_MODE_EMBEDDED) {
    return openEmbedded(options, out_handle);
  }

  if (options.mode == PDCM_MODE_STANDALONE) {
    return PDCM_STATUS_UNAVAILABLE;
  }

  if ((options.flags & PDCM_OPEN_ALLOW_EMBEDDED_FALLBACK) == 0) {
    return PDCM_STATUS_UNAVAILABLE;
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
