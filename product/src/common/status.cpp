#include "common/status.hpp"

namespace pdcm {

const char *statusName(const pdcm_status_t status) noexcept {
  switch (status) {
  case PDCM_STATUS_SUCCESS:
    return "SUCCESS";
  case PDCM_STATUS_PARTIAL_RESULT:
    return "PARTIAL_RESULT";
  case PDCM_STATUS_INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case PDCM_STATUS_UNSUPPORTED:
    return "UNSUPPORTED";
  case PDCM_STATUS_NOT_INITIALIZED:
    return "NOT_INITIALIZED";
  case PDCM_STATUS_UNAVAILABLE:
    return "UNAVAILABLE";
  case PDCM_STATUS_PERMISSION_DENIED:
    return "PERMISSION_DENIED";
  case PDCM_STATUS_TIMEOUT:
    return "TIMEOUT";
  case PDCM_STATUS_RESOURCE_EXHAUSTED:
    return "RESOURCE_EXHAUSTED";
  case PDCM_STATUS_BUFFER_TOO_SMALL:
    return "BUFFER_TOO_SMALL";
  case PDCM_STATUS_NOT_FOUND:
    return "NOT_FOUND";
  case PDCM_STATUS_STALE_GENERATION:
    return "STALE_GENERATION";
  case PDCM_STATUS_INTERNAL:
    return "INTERNAL";
  case PDCM_STATUS_PROTOCOL_INCOMPATIBLE:
    return "PROTOCOL_INCOMPATIBLE";
  }
  return "UNKNOWN";
}

} // namespace pdcm
