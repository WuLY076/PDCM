#include "ipc/handshake.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "pdcm_local.pb.h"

namespace pdcm::ipc {
namespace {

local::v1::CoreState toProtocolState(const CoreState state) {
  switch (state) {
  case CoreState::kCreated:
    return local::v1::CORE_STATE_CREATED;
  case CoreState::kStarting:
    return local::v1::CORE_STATE_STARTING;
  case CoreState::kReady:
    return local::v1::CORE_STATE_READY;
  case CoreState::kDegraded:
    return local::v1::CORE_STATE_DEGRADED;
  case CoreState::kFailed:
    return local::v1::CORE_STATE_FAILED;
  case CoreState::kStopping:
    return local::v1::CORE_STATE_STOPPING;
  case CoreState::kStopped:
    return local::v1::CORE_STATE_STOPPED;
  }
  return local::v1::CORE_STATE_UNSPECIFIED;
}

local::v1::ProviderState toProtocolState(const ProviderState state) {
  switch (state) {
  case ProviderState::kUninitialized:
    return local::v1::PROVIDER_STATE_UNINITIALIZED;
  case ProviderState::kReady:
    return local::v1::PROVIDER_STATE_READY;
  case ProviderState::kUnavailable:
    return local::v1::PROVIDER_STATE_UNAVAILABLE;
  case ProviderState::kShutdown:
    return local::v1::PROVIDER_STATE_SHUTDOWN;
  }
  return local::v1::PROVIDER_STATE_UNSPECIFIED;
}

local::v1::ProviderFailurePhase
toFailurePhase(const CoreDegradedReason reason) {
  switch (reason) {
  case CoreDegradedReason::kProviderUnavailable:
    return local::v1::PROVIDER_FAILURE_PHASE_NATIVE_INITIALIZE;
  case CoreDegradedReason::kDiscoveryFailed:
  case CoreDegradedReason::kTopologyUnsupported:
    return local::v1::PROVIDER_FAILURE_PHASE_DISCOVERY;
  case CoreDegradedReason::kNone:
    return local::v1::PROVIDER_FAILURE_PHASE_NONE;
  }
  return local::v1::PROVIDER_FAILURE_PHASE_NONE;
}

std::uint32_t narrowLimit(const std::size_t value) {
  return static_cast<std::uint32_t>(std::min(
      value,
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
}

std::vector<std::uint8_t> payload(const std::string &serialized) {
  return std::vector<std::uint8_t>(serialized.begin(), serialized.end());
}

} // namespace

HandshakeHandler::HandshakeHandler(SessionManager *sessions)
    : sessions_(sessions) {
  if (sessions_ == nullptr) {
    throw std::invalid_argument("HandshakeHandler requires SessionManager");
  }
}

HandshakeResult HandshakeHandler::handle(const Frame &request,
                                         const CoreSnapshot &core,
                                         const ResourceLimits &limits,
                                         const PeerIdentity &peer) {
  if (request.message_type != MessageType::kHelloRequest) {
    return {Status(PDCM_STATUS_INVALID_ARGUMENT, "first message must be hello"),
            errorFrame(request.request_id, PDCM_STATUS_INVALID_ARGUMENT,
                       "EXPECTED_HELLO"),
            std::nullopt, true};
  }

  if (request.payload.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return {Status(PDCM_STATUS_INVALID_ARGUMENT, "hello payload is too large"),
            errorFrame(request.request_id, PDCM_STATUS_INVALID_ARGUMENT,
                       "HELLO_PAYLOAD_TOO_LARGE"),
            std::nullopt, true};
  }

  local::v1::HelloRequest hello;
  if (!hello.ParseFromArray(request.payload.data(),
                            static_cast<int>(request.payload.size()))) {
    return {Status(PDCM_STATUS_INVALID_ARGUMENT, "malformed hello payload"),
            errorFrame(request.request_id, PDCM_STATUS_INVALID_ARGUMENT,
                       "MALFORMED_HELLO"),
            std::nullopt, true};
  }

  if (request.protocol_major != kProtocolMajor ||
      hello.protocol_major() != kProtocolMajor ||
      hello.protocol_major() != request.protocol_major) {
    return {Status(PDCM_STATUS_PROTOCOL_INCOMPATIBLE,
                   "protocol major is incompatible"),
            errorFrame(request.request_id, PDCM_STATUS_PROTOCOL_INCOMPATIBLE,
                       "INCOMPATIBLE_PROTOCOL_MAJOR"),
            std::nullopt, true};
  }

  if (hello.client_library_version().empty() ||
      hello.client_library_version().size() > 64 || hello.nonce().size() < 16 ||
      hello.nonce().size() > 64 || hello.requested_features_size() > 32 ||
      (peer.process_id != 0 && hello.process_id() != peer.process_id)) {
    return {Status(PDCM_STATUS_INVALID_ARGUMENT, "invalid hello fields"),
            errorFrame(request.request_id, PDCM_STATUS_INVALID_ARGUMENT,
                       "INVALID_HELLO_FIELDS"),
            std::nullopt, true};
  }
  for (const std::string &feature : hello.requested_features()) {
    if (feature.empty() || feature.size() > 64) {
      return {Status(PDCM_STATUS_INVALID_ARGUMENT, "invalid requested feature"),
              errorFrame(request.request_id, PDCM_STATUS_INVALID_ARGUMENT,
                         "INVALID_HELLO_FEATURE"),
              std::nullopt, true};
    }
  }

  SessionCreateResult created = sessions_->beginHandshake(peer);
  if (!created.status.ok()) {
    return {created.status,
            errorFrame(request.request_id, created.status.code(),
                       "SESSION_CAPACITY_UNAVAILABLE"),
            std::nullopt, true};
  }

  const Status activated = sessions_->activate(created.id);
  if (!activated.ok()) {
    (void)sessions_->close(created.id);
    return {activated,
            errorFrame(request.request_id, activated.code(),
                       "SESSION_ACTIVATION_FAILED"),
            std::nullopt, true};
  }

  local::v1::HelloResponse response;
  response.set_protocol_major(kProtocolMajor);
  response.set_protocol_minor(std::min(
      {static_cast<std::uint32_t>(kProtocolMinor), hello.protocol_minor(),
       static_cast<std::uint32_t>(request.protocol_minor)}));
  response.set_session_id(created.id);
  response.set_daemon_version("0.1.0");
  response.set_core_state(toProtocolState(core.state));
  response.set_provider_state(toProtocolState(core.provider_state));
  response.set_provider_failure_phase(toFailurePhase(core.degraded_reason));
  response.set_catalog_generation(core.catalog_generation);
  response.set_detected_device_count(core.detected_device_count);
  response.set_detail_status(static_cast<std::int32_t>(core.detail_status));

  local::v1::ResourceLimits *protocol_limits = response.mutable_limits();
  protocol_limits->set_max_outstanding_requests(
      narrowLimit(limits.max_outstanding_requests_per_session));
  protocol_limits->set_max_watches(narrowLimit(limits.max_watches_per_session));
  protocol_limits->set_max_subscriptions(
      narrowLimit(limits.max_subscriptions_per_session));
  protocol_limits->set_max_frame_bytes(narrowLimit(limits.max_frame_bytes));

  Frame response_frame;
  response_frame.protocol_major = kProtocolMajor;
  response_frame.protocol_minor =
      static_cast<std::uint16_t>(response.protocol_minor());
  response_frame.message_type = MessageType::kHelloResponse;
  response_frame.request_id = request.request_id;
  response_frame.payload = payload(response.SerializeAsString());

  return {Status::success(), std::move(response_frame), created.id, false};
}

Frame HandshakeHandler::errorFrame(const std::uint64_t request_id,
                                   const pdcm_status_t status,
                                   const char *stable_reason) {
  local::v1::ErrorResponse error;
  error.set_status(static_cast<std::int32_t>(status));
  error.set_stable_reason(stable_reason);
  error.set_supported_protocol_major(kProtocolMajor);
  error.set_supported_protocol_minor(kProtocolMinor);

  Frame frame;
  frame.message_type = MessageType::kErrorResponse;
  frame.request_id = request_id;
  frame.payload = payload(error.SerializeAsString());
  return frame;
}

} // namespace pdcm::ipc
