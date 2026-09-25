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
#include "pdcm/types.h"

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
bool validProtocolStatus(const std::int32_t status) {
  return status >= PDCM_STATUS_SUCCESS && status <= PDCM_STATUS_INTERNAL;
}

local::v1::EntityKind toProtocolKind(const EntityKind kind) {
  switch (kind) {
  case EntityKind::kDevice:
    return local::v1::ENTITY_KIND_DEVICE;
  case EntityKind::kNode:
    return local::v1::ENTITY_KIND_NODE;
  case EntityKind::kUnknown:
    return local::v1::ENTITY_KIND_UNSPECIFIED;
  }
  return local::v1::ENTITY_KIND_UNSPECIFIED;
}

EntityKind fromProtocolKind(const local::v1::EntityKind kind) {
  switch (kind) {
  case local::v1::ENTITY_KIND_DEVICE:
    return EntityKind::kDevice;
  case local::v1::ENTITY_KIND_NODE:
    return EntityKind::kNode;
  case local::v1::ENTITY_KIND_UNSPECIFIED:
  default:
    return EntityKind::kUnknown;
  }
}

EntityState fromProtocolState(const local::v1::EntityState state) {
  switch (state) {
  case local::v1::ENTITY_STATE_READY:
    return EntityState::kReady;
  case local::v1::ENTITY_STATE_ERROR:
    return EntityState::kError;
  case local::v1::ENTITY_STATE_UNSPECIFIED:
  default:
    return EntityState::kUnknown;
  }
}

ObservationStatus
fromProtocolStatus(const local::v1::ObservationStatus status) {
  switch (status) {
  case local::v1::OBSERVATION_STATUS_VALID:
    return ObservationStatus::kValid;
  case local::v1::OBSERVATION_STATUS_STALE:
    return ObservationStatus::kStale;
  case local::v1::OBSERVATION_STATUS_NOT_AVAILABLE:
    return ObservationStatus::kNotAvailable;
  case local::v1::OBSERVATION_STATUS_UNSUPPORTED:
    return ObservationStatus::kUnsupported;
  case local::v1::OBSERVATION_STATUS_ERROR:
  case local::v1::OBSERVATION_STATUS_UNSPECIFIED:
  default:
    return ObservationStatus::kError;
  }
}

CapabilityKind fromProtocolKind(const local::v1::CapabilityKind kind) {
  switch (kind) {
  case local::v1::CAPABILITY_KIND_METRICS_CATALOG:
    return CapabilityKind::kMetricsCatalog;
  case local::v1::CAPABILITY_KIND_METRIC:
    return CapabilityKind::kMetric;
  case local::v1::CAPABILITY_KIND_HEALTH:
    return CapabilityKind::kHealth;
  case local::v1::CAPABILITY_KIND_UNSPECIFIED:
  default:
    return CapabilityKind::kMetric;
  }
}

CapabilityReason fromProtocolReason(const local::v1::CapabilityReason reason) {
  switch (reason) {
  case local::v1::CAPABILITY_REASON_SUPPORTED:
    return CapabilityReason::kSupported;
  case local::v1::CAPABILITY_REASON_CATALOG_BLOCKED_EXTERNAL:
    return CapabilityReason::kCatalogBlockedExternal;
  case local::v1::CAPABILITY_REASON_PROVIDER_UNSUPPORTED:
    return CapabilityReason::kProviderUnsupported;
  case local::v1::CAPABILITY_REASON_DEPENDENCY_MISSING:
    return CapabilityReason::kDependencyMissing;
  case local::v1::CAPABILITY_REASON_TEMPORARILY_UNAVAILABLE:
    return CapabilityReason::kTemporarilyUnavailable;
  case local::v1::CAPABILITY_REASON_PERMISSION_HIDDEN:
    return CapabilityReason::kPermissionHidden;
  case local::v1::CAPABILITY_REASON_POST_P0_DISABLED:
    return CapabilityReason::kPostP0Disabled;
  case local::v1::CAPABILITY_REASON_TOPOLOGY_UNSUPPORTED:
    return CapabilityReason::kTopologyUnsupported;
  case local::v1::CAPABILITY_REASON_UNSPECIFIED:
  default:
    return CapabilityReason::kProviderUnsupported;
  }
}

EntityRef fromProtocolEntity(const local::v1::EntityRef &entity) {
  return {fromProtocolKind(entity.kind()), EntityId{entity.pdcm_id()},
          entity.generation()};
}

void fillProtocolEntity(const EntityRef &entity,
                        local::v1::EntityRef *const output) {
  output->set_kind(toProtocolKind(entity.kind));
  output->set_pdcm_id(entity.id.value);
  output->set_generation(entity.generation);
}

bool validEntityRef(const local::v1::EntityRef &entity) {
  return local::v1::EntityKind_IsValid(entity.kind()) &&
         entity.kind() == local::v1::ENTITY_KIND_DEVICE &&
         entity.generation() != 0;
}

bool validEntityInfo(const local::v1::EntityInfo &entity) {
  return entity.has_entity() && validEntityRef(entity.entity()) &&
         local::v1::EntityState_IsValid(entity.state()) &&
         local::v1::ObservationStatus_IsValid(entity.native_id_status()) &&
         local::v1::ObservationStatus_IsValid(entity.pci_bdf_status()) &&
         local::v1::ObservationStatus_IsValid(entity.pdrv_version_status()) &&
         entity.native_id_status() !=
             local::v1::OBSERVATION_STATUS_UNSPECIFIED &&
         entity.pci_bdf_status() != local::v1::OBSERVATION_STATUS_UNSPECIFIED &&
         entity.pdrv_version_status() !=
             local::v1::OBSERVATION_STATUS_UNSPECIFIED &&
         validProtocolStatus(entity.item_status()) &&
         entity.native_id().size() < 64 && entity.pci_bdf().size() < 32 &&
         entity.pdrv_version().size() < 64;
}

bool validCapabilityItem(const local::v1::CapabilityItem &item) {
  return local::v1::CapabilityKind_IsValid(item.kind()) &&
         item.kind() != local::v1::CAPABILITY_KIND_UNSPECIFIED &&
         local::v1::CapabilityReason_IsValid(item.reason()) &&
         item.reason() != local::v1::CAPABILITY_REASON_UNSPECIFIED &&
         item.semantic_version() != 0 && item.catalog_generation() != 0;
}

HealthState fromProtocolState(const local::v1::HealthState state) {
  switch (state) {
  case local::v1::HEALTH_STATE_HEALTHY:
    return HealthState::kHealthy;
  case local::v1::HEALTH_STATE_UNKNOWN:
    return HealthState::kUnknown;
  case local::v1::HEALTH_STATE_WARNING:
    return HealthState::kWarning;
  case local::v1::HEALTH_STATE_ERROR:
    return HealthState::kError;
  case local::v1::HEALTH_STATE_UNSPECIFIED:
  default:
    return HealthState::kUnknown;
  }
}

bool validHealthResponse(const local::v1::HealthQueryResponse &response) {
  return response.has_entity() && validEntityRef(response.entity()) &&
         response.subsystem_id() != 0 &&
         local::v1::HealthState_IsValid(response.state()) &&
         response.state() != local::v1::HEALTH_STATE_UNSPECIFIED &&
         validProtocolStatus(response.item_status()) &&
         response.code() <=
             static_cast<std::uint32_t>(StableHealthCode::kProcessorError) &&
         response.catalog_generation() != 0 &&
         response.source().size() < PDCM_HEALTH_SOURCE_CAPACITY &&
         std::all_of(response.limitations().begin(),
                     response.limitations().end(), [](const std::string &item) {
                       return item.size() < PDCM_HEALTH_LIMITATION_CAPACITY;
                     });
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
Status
StandaloneBackend::exchangeLocked(const ipc::MessageType request_type,
                                  const std::string &request_payload,
                                  const ipc::MessageType expected_response_type,
                                  ipc::Frame *const response) const {
  if (response == nullptr) {
    return Status(PDCM_STATUS_INVALID_ARGUMENT,
                  "standalone response output must not be null");
  }
  if (!started_ || closed_ || !socket_.valid()) {
    return Status(PDCM_STATUS_UNAVAILABLE,
                  "standalone backend is disconnected");
  }
  if (next_request_id_ == 0) {
    return Status(PDCM_STATUS_RESOURCE_EXHAUSTED,
                  "standalone request ID space is exhausted");
  }

  ipc::Frame request;
  request.protocol_major = version_.protocol_major;
  request.protocol_minor = version_.protocol_minor;
  request.message_type = request_type;
  request.request_id = next_request_id_++;
  request.payload.assign(request_payload.begin(), request_payload.end());

  std::vector<std::uint8_t> bytes;
  Status encoded = ipc::encodeFrame(request, max_frame_bytes_, &bytes);
  if (!encoded.ok()) {
    return encoded;
  }

  const MonotonicTime deadline = deadlineAfter(deadline_);
  Status written =
      ipc::writeAll(socket_.get(), bytes.data(), bytes.size(), deadline);
  if (!written.ok()) {
    return written;
  }
  Status read =
      ipc::readOneFrame(socket_.get(), response, max_frame_bytes_, deadline);
  if (!read.ok()) {
    return read;
  }
  if (response->request_id != request.request_id ||
      response->protocol_major != version_.protocol_major ||
      response->protocol_minor != version_.protocol_minor) {
    return Status(PDCM_STATUS_INTERNAL,
                  "local daemon response envelope mismatch");
  }

  if (response->message_type == ipc::MessageType::kErrorResponse) {
    if (response->payload.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return Status(PDCM_STATUS_INTERNAL,
                    "local daemon error payload is too large");
    }
    local::v1::ErrorResponse error;
    if (!error.ParseFromArray(response->payload.data(),
                              static_cast<int>(response->payload.size()))) {
      return Status(PDCM_STATUS_INTERNAL,
                    "local daemon returned malformed error");
    }
    const Status error_status =
        protocolStatus(error.status(), error.stable_reason().c_str());
    return error_status.ok()
               ? Status(PDCM_STATUS_INTERNAL,
                        "local daemon returned a success error response")
               : error_status;
  }

  if (response->message_type != expected_response_type) {
    return Status(PDCM_STATUS_INTERNAL,
                  "local daemon returned unexpected response type");
  }
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

EntityListResult StandaloneBackend::entities(const EntityKind kind) const {
  std::lock_guard<std::mutex> lock(mutex_);

  local::v1::EntityListRequest request;
  request.set_kind(toProtocolKind(kind));

  ipc::Frame response_frame;
  const Status exchanged = exchangeLocked(
      ipc::MessageType::kEntityListRequest, request.SerializeAsString(),
      ipc::MessageType::kEntityListResponse, &response_frame);
  if (!exchanged.ok()) {
    EntityListResult result;
    result.status = exchanged;
    return result;
  }
  if (response_frame.payload.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    EntityListResult result;
    result.status =
        Status(PDCM_STATUS_INTERNAL, "entity response payload is too large");
    return result;
  }

  local::v1::EntityListResponse response;
  if (!response.ParseFromArray(
          response_frame.payload.data(),
          static_cast<int>(response_frame.payload.size())) ||
      !validProtocolStatus(response.status())) {
    EntityListResult result;
    result.status = Status(PDCM_STATUS_INTERNAL,
                           "local daemon returned malformed entities");
    return result;
  }

  EntityListResult result;
  result.status = protocolStatus(response.status(), "entity list result");
  result.detected_device_count = response.detected_device_count();
  result.catalog_generation = response.catalog_generation();
  if (result.status.code() != PDCM_STATUS_SUCCESS &&
      result.status.code() != PDCM_STATUS_PARTIAL_RESULT) {
    if (response.entities_size() != 0) {
      result.status = Status(PDCM_STATUS_INTERNAL,
                             "failed entity response contains records");
    }
    return result;
  }

  result.entities.reserve(static_cast<std::size_t>(response.entities_size()));
  for (const local::v1::EntityInfo &input : response.entities()) {
    if (!validEntityInfo(input)) {
      result.entities.clear();
      result.status =
          Status(PDCM_STATUS_INTERNAL, "local daemon returned invalid entity");
      return result;
    }
    EntityRecord entity;
    entity.ref = fromProtocolEntity(input.entity());
    entity.state = fromProtocolState(input.state());
    entity.item_status = static_cast<pdcm_status_t>(input.item_status());
    entity.native_id = {fromProtocolStatus(input.native_id_status()),
                        input.native_id()};
    entity.pci_bdf = {fromProtocolStatus(input.pci_bdf_status()),
                      input.pci_bdf()};
    entity.pdrv_version = {fromProtocolStatus(input.pdrv_version_status()),
                           input.pdrv_version()};
    result.entities.push_back(std::move(entity));
  }
  return result;
}

CapabilityQueryResult
StandaloneBackend::capabilities(const EntityRef entity) const {
  std::lock_guard<std::mutex> lock(mutex_);

  local::v1::CapabilityQueryRequest request;
  fillProtocolEntity(entity, request.mutable_entity());

  ipc::Frame response_frame;
  const Status exchanged = exchangeLocked(
      ipc::MessageType::kCapabilityQueryRequest, request.SerializeAsString(),
      ipc::MessageType::kCapabilityQueryResponse, &response_frame);
  if (!exchanged.ok()) {
    return {exchanged, std::nullopt};
  }
  if (response_frame.payload.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return {Status(PDCM_STATUS_INTERNAL,
                   "capability response payload is too large"),
            std::nullopt};
  }

  local::v1::CapabilityQueryResponse response;
  if (!response.ParseFromArray(
          response_frame.payload.data(),
          static_cast<int>(response_frame.payload.size())) ||
      !validProtocolStatus(response.status())) {
    return {Status(PDCM_STATUS_INTERNAL,
                   "local daemon returned malformed capabilities"),
            std::nullopt};
  }

  const Status result_status =
      protocolStatus(response.status(), "capability query result");
  if (!result_status.ok()) {
    if (response.has_entity() || response.items_size() != 0 ||
        response.catalog_generation() != 0) {
      return {Status(PDCM_STATUS_INTERNAL,
                     "failed capability response contains a result"),
              std::nullopt};
    }
    return {result_status, std::nullopt};
  }
  if (!response.has_entity() || !validEntityRef(response.entity()) ||
      response.catalog_generation() == 0) {
    return {
        Status(PDCM_STATUS_INTERNAL, "capability response identity is invalid"),
        std::nullopt};
  }

  CapabilitySet capabilities;
  capabilities.entity = fromProtocolEntity(response.entity());
  capabilities.catalog_generation = response.catalog_generation();
  capabilities.items.reserve(static_cast<std::size_t>(response.items_size()));
  for (const local::v1::CapabilityItem &input : response.items()) {
    if (!validCapabilityItem(input) ||
        input.catalog_generation() != response.catalog_generation()) {
      return {Status(PDCM_STATUS_INTERNAL,
                     "local daemon returned invalid capability item"),
              std::nullopt};
    }
    capabilities.items.push_back(
        {fromProtocolKind(input.kind()), input.id(), input.supported(),
         fromProtocolReason(input.reason()), input.semantic_version(),
         input.catalog_generation()});
  }
  return {Status::success(), std::move(capabilities)};
}

HealthQueryResult
StandaloneBackend::health(const HealthRequest &health_request) const {
  std::lock_guard<std::mutex> lock(mutex_);

  local::v1::HealthQueryRequest request;
  fillProtocolEntity(health_request.entity, request.mutable_entity());
  request.set_subsystem_id(health_request.subsystem_id);
  request.set_catalog_generation(health_request.catalog_generation);
  request.set_max_age_ns(health_request.max_age_ns);

  ipc::Frame response_frame;
  const Status exchanged = exchangeLocked(
      ipc::MessageType::kHealthQueryRequest, request.SerializeAsString(),
      ipc::MessageType::kHealthQueryResponse, &response_frame);
  if (!exchanged.ok()) {
    return {exchanged, std::nullopt, std::nullopt};
  }
  if (response_frame.payload.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return {Status(PDCM_STATUS_INTERNAL,
                   "health response payload is too large"),
            std::nullopt, std::nullopt};
  }

  local::v1::HealthQueryResponse response;
  if (!response.ParseFromArray(
          response_frame.payload.data(),
          static_cast<int>(response_frame.payload.size())) ||
      !validProtocolStatus(response.status())) {
    return {Status(PDCM_STATUS_INTERNAL,
                   "local daemon returned malformed health"),
            std::nullopt, std::nullopt};
  }

  const Status result_status =
      protocolStatus(response.status(), "health query result");
  if (result_status.code() != PDCM_STATUS_SUCCESS &&
      result_status.code() != PDCM_STATUS_PARTIAL_RESULT) {
    if (response.has_entity()) {
      return {Status(PDCM_STATUS_INTERNAL,
                     "failed health response contains a result"),
              std::nullopt, std::nullopt};
    }
    return {result_status, std::nullopt, std::nullopt};
  }
  if (!validHealthResponse(response)) {
    return {Status(PDCM_STATUS_INTERNAL,
                   "local daemon returned invalid health result"),
            std::nullopt, std::nullopt};
  }

  HealthResult item;
  item.entity = fromProtocolEntity(response.entity());
  item.subsystem_id = response.subsystem_id();
  item.state = fromProtocolState(response.state());
  item.item_status = fromProtocolStatus(response.item_status());
  item.code = static_cast<StableHealthCode>(response.code());
  item.catalog_generation = response.catalog_generation();
  item.evaluated_monotonic_time_ns =
      response.evaluated_monotonic_time_ns();
  item.evidence_age_ns = response.evidence_age_ns();
  if (response.evidence_id() != 0) {
    EvidenceRef evidence;
    evidence.evidence_id = response.evidence_id();
    evidence.sequence_or_token = response.sequence_or_token();
    evidence.observed_monotonic_time_ns =
        item.evaluated_monotonic_time_ns - item.evidence_age_ns;
    evidence.source.provider = response.source();
    item.evidence.push_back(std::move(evidence));
  }
  for (const std::string &detail : response.limitations()) {
    item.limitations.push_back(
        {HealthLimitationCode::kEvidenceMissing, detail});
  }
  return {result_status, std::move(item), std::nullopt};
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
