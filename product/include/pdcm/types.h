#ifndef PDCM_TYPES_H_
#define PDCM_TYPES_H_

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

PDCM_END_DECLS

#endif // PDCM_TYPES_H_
