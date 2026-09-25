#ifndef PDCM_TYPES_H_
#define PDCM_TYPES_H_

#include <stddef.h>
#include <stdint.h>

#include "pdcm/base.h"

PDCM_BEGIN_DECLS

typedef uint64_t pdcm_entity_id_t;
typedef uint32_t pdcm_metric_id_t;

typedef enum pdcm_entity_kind {
  PDCM_ENTITY_KIND_UNKNOWN = 0,
  PDCM_ENTITY_KIND_DEVICE = 1,
  PDCM_ENTITY_KIND_NODE = 2
} pdcm_entity_kind_t;

typedef struct pdcm_entity_ref {
  pdcm_struct_header_t header;
  uint32_t kind;
  uint32_t reserved;
  pdcm_entity_id_t pdcm_id;
  uint64_t generation;
} pdcm_entity_ref_t;
#define PDCM_NATIVE_ID_CAPACITY 64u
#define PDCM_PCI_BDF_CAPACITY 32u
#define PDCM_VERSION_STRING_CAPACITY 64u

#define PDCM_ENTITY_REF_INIT                                                   \
  {                                                                            \
    {(uint32_t)sizeof(pdcm_entity_ref_t), PDCM_STRUCT_VERSION_1},              \
        (uint32_t)PDCM_ENTITY_KIND_UNKNOWN, 0u, UINT64_C(0), UINT64_C(0)       \
  }

typedef enum pdcm_entity_state {
  PDCM_ENTITY_STATE_UNKNOWN = 0,
  PDCM_ENTITY_STATE_READY = 1,
  PDCM_ENTITY_STATE_ERROR = 2
} pdcm_entity_state_t;

typedef struct pdcm_entity_filter {
  pdcm_struct_header_t header;
  uint32_t kind;
  uint32_t reserved;
} pdcm_entity_filter_t;

#define PDCM_ENTITY_FILTER_INIT                                                \
  {                                                                            \
    {(uint32_t)sizeof(pdcm_entity_filter_t), PDCM_STRUCT_VERSION_1},           \
        (uint32_t)PDCM_ENTITY_KIND_UNKNOWN, 0u                                 \
  }

typedef struct pdcm_entity_info {
  pdcm_struct_header_t header;
  pdcm_entity_ref_t entity;
  uint32_t state;
  int32_t item_status;
  uint32_t native_id_status;
  uint32_t pci_bdf_status;
  uint32_t pdrv_version_status;
  uint32_t reserved;
  char native_id[PDCM_NATIVE_ID_CAPACITY];
  char pci_bdf[PDCM_PCI_BDF_CAPACITY];
  char pdrv_version[PDCM_VERSION_STRING_CAPACITY];
} pdcm_entity_info_t;

#define PDCM_ENTITY_INFO_INIT                                                  \
  {                                                                            \
    {(uint32_t)sizeof(pdcm_entity_info_t), PDCM_STRUCT_VERSION_1},             \
        PDCM_ENTITY_REF_INIT, (uint32_t)PDCM_ENTITY_STATE_UNKNOWN, (int32_t)0, \
        (uint32_t)PDCM_OBSERVATION_NOT_AVAILABLE,                              \
        (uint32_t)PDCM_OBSERVATION_NOT_AVAILABLE,                              \
        (uint32_t)PDCM_OBSERVATION_NOT_AVAILABLE, 0u, {0}, {0}, {              \
      0                                                                        \
    }                                                                          \
  }

typedef enum pdcm_capability_kind {
  PDCM_CAPABILITY_METRICS_CATALOG = 1,
  PDCM_CAPABILITY_METRIC = 2,
  PDCM_CAPABILITY_HEALTH = 3
} pdcm_capability_kind_t;

typedef enum pdcm_capability_reason {
  PDCM_CAPABILITY_REASON_SUPPORTED = 0,
  PDCM_CAPABILITY_REASON_CATALOG_BLOCKED_EXTERNAL = 1,
  PDCM_CAPABILITY_REASON_PROVIDER_UNSUPPORTED = 2,
  PDCM_CAPABILITY_REASON_DEPENDENCY_MISSING = 3,
  PDCM_CAPABILITY_REASON_TEMPORARILY_UNAVAILABLE = 4,
  PDCM_CAPABILITY_REASON_PERMISSION_HIDDEN = 5,
  PDCM_CAPABILITY_REASON_POST_P0_DISABLED = 6,
  PDCM_CAPABILITY_REASON_TOPOLOGY_UNSUPPORTED = 7
} pdcm_capability_reason_t;

typedef struct pdcm_capability_item {
  pdcm_struct_header_t header;
  uint32_t kind;
  uint32_t id;
  uint32_t supported;
  uint32_t reason;
  uint32_t semantic_version;
  uint32_t reserved;
  uint64_t catalog_generation;
} pdcm_capability_item_t;

#define PDCM_CAPABILITY_ITEM_INIT                                              \
  {                                                                            \
    {(uint32_t)sizeof(pdcm_capability_item_t), PDCM_STRUCT_VERSION_1}, 0u, 0u, \
        0u, 0u, 0u, 0u, UINT64_C(0)                                            \
  }

typedef struct pdcm_capability_set {
  pdcm_struct_header_t header;
  pdcm_entity_ref_t entity;
  pdcm_capability_item_t *items;
  size_t item_capacity;
  size_t item_count;
  uint64_t catalog_generation;
} pdcm_capability_set_t;

#define PDCM_CAPABILITY_SET_INIT                                               \
  {                                                                            \
    {(uint32_t)sizeof(pdcm_capability_set_t), PDCM_STRUCT_VERSION_1},          \
        PDCM_ENTITY_REF_INIT, (pdcm_capability_item_t *)0, 0u, 0u, UINT64_C(0) \
  }

typedef enum pdcm_observation_status {
  PDCM_OBSERVATION_VALID = 0,
  PDCM_OBSERVATION_STALE = 1,
  PDCM_OBSERVATION_NOT_AVAILABLE = 2,
  PDCM_OBSERVATION_UNSUPPORTED = 3,
  PDCM_OBSERVATION_ERROR = 4
} pdcm_observation_status_t;

typedef enum pdcm_metric_value_type {
  PDCM_VALUE_TYPE_INT64 = 0,
  PDCM_VALUE_TYPE_UINT64 = 1,
  PDCM_VALUE_TYPE_DOUBLE = 2,
  PDCM_VALUE_TYPE_BOOL = 3,
  PDCM_VALUE_TYPE_STRING = 4,
  PDCM_VALUE_TYPE_ENUM = 5
} pdcm_metric_value_type_t;

typedef enum pdcm_health_subsystem {
  PDCM_HEALTH_SUBSYSTEM_FIRMWARE_HEARTBEAT = 1
} pdcm_health_subsystem_t;

typedef enum pdcm_health_state {
  PDCM_HEALTH_STATE_UNKNOWN = 0,
  PDCM_HEALTH_STATE_HEALTHY = 1,
  PDCM_HEALTH_STATE_WARNING = 2,
  PDCM_HEALTH_STATE_ERROR = 3
} pdcm_health_state_t;

typedef enum pdcm_health_code {
  PDCM_HEALTH_CODE_HEARTBEAT_OK = 0,
  PDCM_HEALTH_CODE_HEARTBEAT_WARNING = 1,
  PDCM_HEALTH_CODE_HEARTBEAT_FAULT = 2,
  PDCM_HEALTH_CODE_HEARTBEAT_MISSING = 3,
  PDCM_HEALTH_CODE_HEARTBEAT_STALE = 4,
  PDCM_HEALTH_CODE_HEARTBEAT_TIMEOUT = 5,
  PDCM_HEALTH_CODE_HEARTBEAT_READ_ERROR = 6,
  PDCM_HEALTH_CODE_HEARTBEAT_UNSUPPORTED = 7,
  PDCM_HEALTH_CODE_PROVIDER_UNAVAILABLE = 8,
  PDCM_HEALTH_CODE_PROCESSOR_ERROR = 9
} pdcm_health_code_t;

#define PDCM_HEALTH_SOURCE_CAPACITY 64u
#define PDCM_HEALTH_LIMITATION_CAPACITY 160u

typedef struct pdcm_health_request {
  pdcm_struct_header_t header;
  pdcm_entity_ref_t entity;
  uint32_t subsystem_id;
  uint32_t reserved;
  uint64_t catalog_generation;
  uint64_t max_age_ns;
} pdcm_health_request_t;

#define PDCM_HEALTH_REQUEST_INIT                                              \
  {                                                                            \
    {(uint32_t)sizeof(pdcm_health_request_t), PDCM_STRUCT_VERSION_1},          \
        PDCM_ENTITY_REF_INIT,                                                  \
        (uint32_t)PDCM_HEALTH_SUBSYSTEM_FIRMWARE_HEARTBEAT, 0u, UINT64_C(0),  \
        UINT64_C(0)                                                            \
  }

typedef struct pdcm_health_result {
  pdcm_struct_header_t header;
  pdcm_entity_ref_t entity;
  uint32_t subsystem_id;
  uint32_t state;
  uint32_t item_status;
  uint32_t code;
  uint64_t catalog_generation;
  int64_t evaluated_monotonic_time_ns;
  int64_t evidence_age_ns;
  uint64_t evidence_id;
  uint64_t sequence_or_token;
  char source[PDCM_HEALTH_SOURCE_CAPACITY];
  char limitation[PDCM_HEALTH_LIMITATION_CAPACITY];
} pdcm_health_result_t;

#define PDCM_HEALTH_RESULT_INIT                                               \
  {                                                                            \
    {(uint32_t)sizeof(pdcm_health_result_t), PDCM_STRUCT_VERSION_1},           \
        PDCM_ENTITY_REF_INIT, 0u, (uint32_t)PDCM_HEALTH_STATE_UNKNOWN,         \
        (uint32_t)PDCM_OBSERVATION_NOT_AVAILABLE,                              \
        (uint32_t)PDCM_HEALTH_CODE_HEARTBEAT_MISSING, UINT64_C(0), INT64_C(0),\
        INT64_C(0), UINT64_C(0), UINT64_C(0), {0}, {0}                         \
  }

PDCM_END_DECLS

#endif // PDCM_TYPES_H_
