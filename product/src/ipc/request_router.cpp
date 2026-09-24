#include "ipc/request_router.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "proto/local/pdcm_local.pb.h"

namespace pdcm::ipc {
namespace {

std::vector<std::uint8_t> payload(const std::string &serialized) {
  return std::vector<std::uint8_t>(serialized.begin(), serialized.end());
}

Frame responseFrame(const Frame &request, const MessageType type,
                    const std::string &serialized) {
  Frame response;
  response.protocol_major = kProtocolMajor;
  response.protocol_minor = std::min(
      request.protocol_minor, static_cast<std::uint16_t>(kProtocolMinor));
  response.message_type = type;
  response.request_id = request.request_id;
  response.payload = payload(serialized);
  return response;
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

local::v1::EntityState toProtocolState(const EntityState state) {
  switch (state) {
  case EntityState::kReady:
    return local::v1::ENTITY_STATE_READY;
  case EntityState::kError:
    return local::v1::ENTITY_STATE_ERROR;
  case EntityState::kUnknown:
    return local::v1::ENTITY_STATE_UNSPECIFIED;
  }
  return local::v1::ENTITY_STATE_UNSPECIFIED;
}

local::v1::ObservationStatus toProtocolStatus(const ObservationStatus status) {
  switch (status) {
  case ObservationStatus::kValid:
    return local::v1::OBSERVATION_STATUS_VALID;
  case ObservationStatus::kStale:
    return local::v1::OBSERVATION_STATUS_STALE;
  case ObservationStatus::kNotAvailable:
    return local::v1::OBSERVATION_STATUS_NOT_AVAILABLE;
  case ObservationStatus::kUnsupported:
    return local::v1::OBSERVATION_STATUS_UNSUPPORTED;
  case ObservationStatus::kError:
    return local::v1::OBSERVATION_STATUS_ERROR;
  }
  return local::v1::OBSERVATION_STATUS_ERROR;
}

local::v1::CapabilityKind toProtocolKind(const CapabilityKind kind) {
  switch (kind) {
  case CapabilityKind::kMetricsCatalog:
    return local::v1::CAPABILITY_KIND_METRICS_CATALOG;
  case CapabilityKind::kMetric:
    return local::v1::CAPABILITY_KIND_METRIC;
  case CapabilityKind::kHealth:
    return local::v1::CAPABILITY_KIND_HEALTH;
  }
  return local::v1::CAPABILITY_KIND_UNSPECIFIED;
}

local::v1::CapabilityReason toProtocolReason(const CapabilityReason reason) {
  switch (reason) {
  case CapabilityReason::kSupported:
    return local::v1::CAPABILITY_REASON_SUPPORTED;
  case CapabilityReason::kCatalogBlockedExternal:
    return local::v1::CAPABILITY_REASON_CATALOG_BLOCKED_EXTERNAL;
  case CapabilityReason::kProviderUnsupported:
    return local::v1::CAPABILITY_REASON_PROVIDER_UNSUPPORTED;
  case CapabilityReason::kDependencyMissing:
    return local::v1::CAPABILITY_REASON_DEPENDENCY_MISSING;
  case CapabilityReason::kTemporarilyUnavailable:
    return local::v1::CAPABILITY_REASON_TEMPORARILY_UNAVAILABLE;
  case CapabilityReason::kPermissionHidden:
    return local::v1::CAPABILITY_REASON_PERMISSION_HIDDEN;
  case CapabilityReason::kPostP0Disabled:
    return local::v1::CAPABILITY_REASON_POST_P0_DISABLED;
  case CapabilityReason::kTopologyUnsupported:
    return local::v1::CAPABILITY_REASON_TOPOLOGY_UNSUPPORTED;
  }
  return local::v1::CAPABILITY_REASON_UNSPECIFIED;
}

void fillEntityRef(const EntityRef &source,
                   local::v1::EntityRef *const destination) {
  destination->set_kind(toProtocolKind(source.kind));
  destination->set_pdcm_id(source.id.value);
  destination->set_generation(source.generation);
}

bool validRequestPayload(const Frame &request) {
  return request.payload.size() <=
         static_cast<std::size_t>(std::numeric_limits<int>::max());
}

} // namespace

RequestRouter::RequestRouter(PdcmServiceCore *const core) : core_(core) {
  if (core_ == nullptr) {
    throw std::invalid_argument("RequestRouter requires service core");
  }
}

RequestRouteResult RequestRouter::route(const Frame &request) const {
  if (request.protocol_major != kProtocolMajor ||
      request.protocol_minor > kProtocolMinor) {
    return {errorFrame(request.request_id, PDCM_STATUS_UNSUPPORTED,
                       "INCOMPATIBLE_PROTOCOL"),
            true};
  }
  if (request.request_id == 0 || request.flags != 0 ||
      !validRequestPayload(request)) {
    return {errorFrame(request.request_id, PDCM_STATUS_INVALID_ARGUMENT,
                       "INVALID_FRAME_FIELDS"),
            true};
  }

  if (request.message_type == MessageType::kEntityListRequest) {
    local::v1::EntityListRequest protocol_request;
    if (!protocol_request.ParseFromArray(
            request.payload.data(), static_cast<int>(request.payload.size())) ||
        !local::v1::EntityKind_IsValid(protocol_request.kind())) {
      return {errorFrame(request.request_id, PDCM_STATUS_INVALID_ARGUMENT,
                         "MALFORMED_ENTITY_LIST_REQUEST"),
              true};
    }

    const EntityListResult result =
        core_->entities(fromProtocolKind(protocol_request.kind()));
    local::v1::EntityListResponse response;
    response.set_status(static_cast<std::int32_t>(result.status.code()));
    response.set_detected_device_count(result.detected_device_count);
    response.set_catalog_generation(result.catalog_generation);
    for (const EntityRecord &entity : result.entities) {
      local::v1::EntityInfo *const output = response.add_entities();
      fillEntityRef(entity.ref, output->mutable_entity());
      output->set_state(toProtocolState(entity.state));
      output->set_item_status(static_cast<std::int32_t>(entity.item_status));
      output->set_native_id_status(toProtocolStatus(entity.native_id.status));
      output->set_pci_bdf_status(toProtocolStatus(entity.pci_bdf.status));
      output->set_pdrv_version_status(
          toProtocolStatus(entity.pdrv_version.status));
      output->set_native_id(entity.native_id.value);
      output->set_pci_bdf(entity.pci_bdf.value);
      output->set_pdrv_version(entity.pdrv_version.value);
    }
    return {responseFrame(request, MessageType::kEntityListResponse,
                          response.SerializeAsString()),
            false};
  }

  if (request.message_type == MessageType::kCapabilityQueryRequest) {
    local::v1::CapabilityQueryRequest protocol_request;
    if (!protocol_request.ParseFromArray(
            request.payload.data(), static_cast<int>(request.payload.size())) ||
        !protocol_request.has_entity() ||
        !local::v1::EntityKind_IsValid(protocol_request.entity().kind()) ||
        protocol_request.entity().kind() != local::v1::ENTITY_KIND_DEVICE ||
        protocol_request.entity().generation() == 0) {
      return {errorFrame(request.request_id, PDCM_STATUS_INVALID_ARGUMENT,
                         "MALFORMED_CAPABILITY_QUERY_REQUEST"),
              true};
    }

    const EntityRef entity{fromProtocolKind(protocol_request.entity().kind()),
                           EntityId{protocol_request.entity().pdcm_id()},
                           protocol_request.entity().generation()};
    const CapabilityQueryResult result = core_->capabilities(entity);
    local::v1::CapabilityQueryResponse response;
    response.set_status(static_cast<std::int32_t>(result.status.code()));
    if (result.capabilities.has_value()) {
      fillEntityRef(result.capabilities->entity, response.mutable_entity());
      response.set_catalog_generation(result.capabilities->catalog_generation);
      for (const CapabilityItem &item : result.capabilities->items) {
        local::v1::CapabilityItem *const output = response.add_items();
        output->set_kind(toProtocolKind(item.kind));
        output->set_id(item.id);
        output->set_supported(item.supported);
        output->set_reason(toProtocolReason(item.reason));
        output->set_semantic_version(item.semantic_version);
        output->set_catalog_generation(item.catalog_generation);
      }
    }
    return {responseFrame(request, MessageType::kCapabilityQueryResponse,
                          response.SerializeAsString()),
            false};
  }

  return {errorFrame(request.request_id, PDCM_STATUS_UNSUPPORTED,
                     "UNSUPPORTED_MESSAGE_TYPE"),
          false};
}

Frame RequestRouter::errorFrame(const std::uint64_t request_id,
                                const pdcm_status_t status,
                                const char *const stable_reason) {
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
