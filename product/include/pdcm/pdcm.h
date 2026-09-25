#ifndef PDCM_PDCM_H_
#define PDCM_PDCM_H_

#include <stdint.h>

#include "pdcm/base.h"
#include "pdcm/status.h"
#include "pdcm/types.h"

PDCM_BEGIN_DECLS

typedef struct pdcm_handle pdcm_handle_t;

typedef enum pdcm_mode {
  PDCM_MODE_STANDALONE = 1,
  PDCM_MODE_EMBEDDED = 2,
  PDCM_MODE_AUTO = 3
} pdcm_mode_t;

typedef enum pdcm_target {
  PDCM_TARGET_UNKNOWN = 0,
  PDCM_TARGET_FPGA = 1,
  PDCM_TARGET_EMU = 2
} pdcm_target_t;

typedef enum pdcm_core_state {
  PDCM_CORE_STATE_UNKNOWN = 0,
  PDCM_CORE_STATE_CREATED = 1,
  PDCM_CORE_STATE_STARTING = 2,
  PDCM_CORE_STATE_READY = 3,
  PDCM_CORE_STATE_DEGRADED = 4,
  PDCM_CORE_STATE_FAILED = 5,
  PDCM_CORE_STATE_STOPPING = 6,
  PDCM_CORE_STATE_STOPPED = 7
} pdcm_core_state_t;

typedef enum pdcm_provider_state {
  PDCM_PROVIDER_STATE_UNKNOWN = 0,
  PDCM_PROVIDER_STATE_UNINITIALIZED = 1,
  PDCM_PROVIDER_STATE_READY = 2,
  PDCM_PROVIDER_STATE_UNAVAILABLE = 3,
  PDCM_PROVIDER_STATE_SHUTDOWN = 4
} pdcm_provider_state_t;

typedef enum pdcm_provider_load_state {
  PDCM_PROVIDER_LOAD_NOT_ATTEMPTED = 0,
  PDCM_PROVIDER_LOAD_LOADED = 1,
  PDCM_PROVIDER_LOAD_FAILED = 2
} pdcm_provider_load_state_t;

typedef enum pdcm_provider_compatibility {
  PDCM_PROVIDER_COMPATIBILITY_UNKNOWN = 0,
  PDCM_PROVIDER_COMPATIBILITY_COMPATIBLE = 1,
  PDCM_PROVIDER_COMPATIBILITY_UNSUPPORTED_ABI = 2
} pdcm_provider_compatibility_t;

typedef enum pdcm_provider_failure_phase {
  PDCM_PROVIDER_FAILURE_NONE = 0,
  PDCM_PROVIDER_FAILURE_LOAD = 1,
  PDCM_PROVIDER_FAILURE_ENTRY = 2,
  PDCM_PROVIDER_FAILURE_ABI = 3,
  PDCM_PROVIDER_FAILURE_NATIVE_INITIALIZE = 4,
  PDCM_PROVIDER_FAILURE_DISCOVERY = 5
} pdcm_provider_failure_phase_t;

enum { PDCM_OPEN_ALLOW_EMBEDDED_FALLBACK = 1u << 0 };

typedef struct pdcm_open_options {
  pdcm_struct_header_t header;
  uint32_t mode;
  uint32_t flags;
  const char *endpoint;
  uint64_t deadline_ns;
  uint32_t required_protocol_major;
  uint32_t required_protocol_minor;
  uint32_t target;
  uint32_t reserved;
} pdcm_open_options_t;

#define PDCM_OPEN_OPTIONS_INIT                                                 \
  {                                                                            \
    {(uint32_t)sizeof(pdcm_open_options_t), PDCM_STRUCT_VERSION_1},            \
        (uint32_t)PDCM_MODE_STANDALONE, 0u, (const char *)0, 0u, 1u, 0u,       \
        (uint32_t)PDCM_TARGET_UNKNOWN, 0u                                      \
  }

typedef struct pdcm_version_info {
  pdcm_struct_header_t header;
  char library_version[32];
  char daemon_version[32];
  uint32_t public_abi_major;
  uint32_t public_abi_minor;
  uint32_t local_protocol_major;
  uint32_t local_protocol_minor;
  uint32_t negotiated_protocol_major;
  uint32_t negotiated_protocol_minor;
  uint32_t mode;
  uint32_t target;
  uint32_t core_state;
  uint32_t provider_state;
  uint32_t provider_load_state;
  uint32_t provider_compatibility;
  uint32_t provider_failure_phase;
  int32_t detail_status;
  uint32_t detected_device_count;
  uint32_t reserved;
  uint64_t session_id;
  uint64_t catalog_generation;
} pdcm_version_info_t;

#define PDCM_VERSION_INFO_INIT                                                 \
  {                                                                            \
    {(uint32_t)sizeof(pdcm_version_info_t), PDCM_STRUCT_VERSION_1}, {0}, {0},  \
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0, 0u, 0u,         \
        UINT64_C(0), UINT64_C(0)                                               \
  }

PDCM_API pdcm_status_t pdcm_open(const pdcm_open_options_t *options,
                                 pdcm_handle_t **out_handle);

PDCM_API pdcm_status_t pdcm_version_get(pdcm_handle_t *handle,
                                        pdcm_version_info_t *out_version);

PDCM_API pdcm_status_t pdcm_entity_list(pdcm_handle_t *handle,
                                        const pdcm_entity_filter_t *filter,
                                        pdcm_entity_info_t *entities,
                                        size_t *inout_count);

PDCM_API pdcm_status_t
pdcm_capability_query(pdcm_handle_t *handle, const pdcm_entity_ref_t *entity,
                      pdcm_capability_set_t *out_capabilities);

PDCM_API pdcm_status_t
pdcm_health_query(pdcm_handle_t *handle, const pdcm_health_request_t *request,
                  pdcm_health_result_t *out_result);

PDCM_API pdcm_status_t pdcm_close(pdcm_handle_t **handle);

PDCM_END_DECLS

#endif // PDCM_PDCM_H_
