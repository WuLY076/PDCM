#include "client/standalone_backend.hpp"

#include <array>
#include <chrono>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "ipc/frame.hpp"
#include "proto/local/pdcm_local.pb.h"

namespace pdcm {
namespace {

CoreState fromProtocolState(const local::v1::CoreState state) {
  switch (state) {
  case local::v1::CORE_STATE_CREATED:
    return CoreState::kCreated;
  case local::v1::CORE_STATE_STARTING:
    return CoreState::kStarting;
  case local::v1::CORE_STATE_READY:
    return CoreState::kReady;
  case local::v1::CORE_STATE_DEGRADED:
    return CoreState::kDegraded;
  case local::v1::CORE_STATE_FAILED:
    return CoreState::kFailed;
  case local::v1::CORE_STATE_STOPPING:
    return CoreState::kStopping;
  case local::v1::CORE_STATE_STOPPED:
    return CoreState::kStopped;
  case local::v1::CORE_STATE_UNSPECIFIED:
  default:
    return CoreState::kCreated;
  }
}

ProviderState fromProtocolState(const local::v1::ProviderState state) {
  switch (state) {
  case local::v1::PROVIDER_STATE_UNINITIALIZED:
    return ProviderState::kUninitialized;
  case local::v1::PROVIDER_STATE_READY:
    return ProviderState::kReady;
  case local::v1::PROVIDER_STATE_UNAVAILABLE:
    return ProviderState::kUnavailable;
  case local::v1::PROVIDER_STATE_SHUTDOWN:
    return ProviderState::kShutdown;
  case local::v1::PROVIDER_STATE_UNSPECIFIED:
  default:
    return ProviderState::kUninitialized;
  }
}

ProviderFailurePhase
fromProtocolPhase(const local::v1::ProviderFailurePhase phase) {
  switch (phase) {
  case local::v1::PROVIDER_FAILURE_PHASE_NONE:
    return ProviderFailurePhase::kNone;
  case local::v1::PROVIDER_FAILURE_PHASE_LOAD:
    return ProviderFailurePhase::kLoad;
  case local::v1::PROVIDER_FAILURE_PHASE_ENTRY:
    return ProviderFailurePhase::kEntry;
  case local::v1::PROVIDER_FAILURE_PHASE_ABI:
    return ProviderFailurePhase::kAbi;
  case local::v1::PROVIDER_FAILURE_PHASE_NATIVE_INITIALIZE:
    return ProviderFailurePhase::kNativeInitialize;
  case local::v1::PROVIDER_FAILURE_PHASE_DISCOVERY:
    return ProviderFailurePhase::kDiscovery;
  default:
    return ProviderFailurePhase::kNone;
  }
}

Status protocolStatus(const std::int32_t raw_status,
                      const char *const message) {
  if (raw_status < PDCM_STATUS_SUCCESS || raw_status > PDCM_STATUS_INTERNAL) {
    return Status(PDCM_STATUS_INTERNAL, "invalid status from local daemon");
  }
  return Status(static_cast<pdcm_status_t>(raw_status), message);
}

MonotonicTime deadlineAfter(const std::chrono::milliseconds timeout) {
  return std::chrono::time_point_cast<Nanoseconds>(
             std::chrono::steady_clock::now()) +
         timeout;
}

std::string createNonce() {
  std::array<char, 16> nonce{};
  std::random_device random;
  for (char &value : nonce) {
    value = static_cast<char>(random() & 0xFFU);
  }
  return std::string(nonce.data(), nonce.size());
}

} // namespace

StandaloneBackend::StandaloneBackend(std::string endpoint,
                                     const std::chrono::milliseconds deadline,
                                     const std::size_t max_frame_bytes)
    : endpoint_(std::move(endpoint)), deadline_(deadline),
      max_frame_bytes_(max_frame_bytes) {}

StandaloneBackend::~StandaloneBackend() { (void)close(); }

Status StandaloneBackend::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) {
    return Status(PDCM_STATUS_NOT_INITIALIZED,
                  "standalone backend is already closed");
  }
  if (started_) {
    return Status(PDCM_STATUS_NOT_INITIALIZED,
                  "standalone backend is already started");
  }

  const MonotonicTime deadline = deadlineAfter(deadline_);
  Status connected = ipc::connectUnixSocket(endpoint_, deadline, &socket_);
  if (!connected.ok()) {
    return connected;
  }

  local::v1::HelloRequest hello;
  hello.set_protocol_major(ipc::kProtocolMajor);
  hello.set_protocol_minor(ipc::kProtocolMinor);
  hello.set_client_library_version("0.1.0");
  hello.set_process_id(static_cast<std::uint64_t>(::getpid()));
  hello.set_nonce(createNonce());

  const std::string serialized = hello.SerializeAsString();
  ipc::Frame request;
  request.message_type = ipc::MessageType::kHelloRequest;
  request.request_id = 1;
  request.payload.assign(serialized.begin(), serialized.end());

  std::vector<std::uint8_t> bytes;
  Status encoded = ipc::encodeFrame(request, max_frame_bytes_, &bytes);
  if (!encoded.ok()) {
    socket_.reset();
    return encoded;
  }
  Status written =
      ipc::writeAll(socket_.get(), bytes.data(), bytes.size(), deadline);
  if (!written.ok()) {
    socket_.reset();
    return written;
  }

  ipc::Frame response_frame;
  Status read = ipc::readOneFrame(socket_.get(), &response_frame,
                                  max_frame_bytes_, deadline);
  if (!read.ok()) {
    socket_.reset();
    return read;
  }
  if (response_frame.request_id != request.request_id) {
    socket_.reset();
    return Status(PDCM_STATUS_INTERNAL,
                  "local daemon response request ID mismatch");
  }

  if (response_frame.message_type == ipc::MessageType::kErrorResponse) {
    if (response_frame.payload.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      socket_.reset();
      return Status(PDCM_STATUS_INTERNAL,
                    "local daemon error payload is too large");
    }
    local::v1::ErrorResponse error;
    if (!error.ParseFromArray(
            response_frame.payload.data(),
            static_cast<int>(response_frame.payload.size()))) {
      socket_.reset();
      return Status(PDCM_STATUS_INTERNAL,
                    "local daemon returned malformed error");
    }
    socket_.reset();
    const Status error_status =
        protocolStatus(error.status(), error.stable_reason().c_str());
    return error_status.ok()
               ? Status(PDCM_STATUS_INTERNAL,
                        "local daemon returned a success error response")
               : error_status;
  }

  if (response_frame.message_type != ipc::MessageType::kHelloResponse ||
      response_frame.payload.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    socket_.reset();
    return Status(PDCM_STATUS_INTERNAL,
                  "local daemon returned invalid handshake response");
  }

  local::v1::HelloResponse response;
  if (!response.ParseFromArray(
          response_frame.payload.data(),
          static_cast<int>(response_frame.payload.size()))) {
    socket_.reset();
    return Status(PDCM_STATUS_INTERNAL,
                  "local daemon returned malformed handshake");
  }
  if (response.protocol_major() != ipc::kProtocolMajor ||
      response.protocol_minor() > ipc::kProtocolMinor ||
      response_frame.protocol_major != response.protocol_major() ||
      response_frame.protocol_minor != response.protocol_minor() ||
      response.session_id() == 0 ||
      response.core_state() == local::v1::CORE_STATE_UNSPECIFIED ||
      response.provider_state() == local::v1::PROVIDER_STATE_UNSPECIFIED ||
      !local::v1::CoreState_IsValid(response.core_state()) ||
      !local::v1::ProviderState_IsValid(response.provider_state()) ||
      !local::v1::ProviderFailurePhase_IsValid(
          response.provider_failure_phase())) {
    socket_.reset();
    return Status(PDCM_STATUS_UNSUPPORTED,
                  "local daemon negotiated an invalid protocol");
  }

  version_.daemon_version = response.daemon_version();
  version_.mode = BackendMode::kStandalone;
  version_.core.state = fromProtocolState(response.core_state());
  version_.core.provider_state = fromProtocolState(response.provider_state());
  version_.core.detail_status =
      protocolStatus(response.detail_status(), "daemon detail status").code();
  version_.core.catalog_generation = response.catalog_generation();
  version_.core.detected_device_count = response.detected_device_count();
  version_.provider_failure_phase =
      fromProtocolPhase(response.provider_failure_phase());
  version_.provider_load_attempted =
      response.provider_state() != local::v1::PROVIDER_STATE_UNINITIALIZED;
  version_.provider_loaded =
      response.provider_state() == local::v1::PROVIDER_STATE_READY;
  version_.provider_abi_compatible = version_.provider_loaded;
  version_.protocol_major =
      static_cast<std::uint16_t>(response.protocol_major());
  version_.protocol_minor =
      static_cast<std::uint16_t>(response.protocol_minor());
  version_.session_id = response.session_id();
  started_ = true;
  return Status::success();
}

Status StandaloneBackend::version(BackendVersion *const version) const {
  if (version == nullptr) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "version output must not be null");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || closed_ || !socket_.valid()) {
    return Status(PDCM_STATUS_UNAVAILABLE,
                  "standalone backend is disconnected");
  }
  Status connected = ipc::checkUnixSocketConnected(socket_.get());
  if (!connected.ok()) {
    return connected;
  }
  *version = version_;
  return Status::success();
}

EntityListResult StandaloneBackend::entities(const EntityKind) const {
  EntityListResult result;
  result.status = Status(PDCM_STATUS_UNSUPPORTED,
                         "standalone discovery routing is not initialized");
  return result;
}

CapabilityQueryResult StandaloneBackend::capabilities(const EntityRef) const {
  return {Status(PDCM_STATUS_UNSUPPORTED,
                 "standalone capability routing is not initialized"),
          std::nullopt};
}

Status StandaloneBackend::close() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) {
    return Status::success();
  }
  socket_.reset();
  started_ = false;
  closed_ = true;
  return Status::success();
}

} // namespace pdcm
