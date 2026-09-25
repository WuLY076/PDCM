#include <stddef.h>
#include <stdint.h>

#include "pdcm/base.h"
#include "pdcm/pdcm.h"
#include "pdcm/status.h"
#include "pdcm/types.h"

_Static_assert(PDCM_STATUS_SUCCESS == 0, "success status must remain zero");
_Static_assert(sizeof(pdcm_struct_header_t) == 8,
               "public structure header layout changed");
_Static_assert(offsetof(pdcm_entity_ref_t, pdcm_id) == 16,
               "entity reference layout changed");

int main(void) {
  pdcm_open_options_t options = PDCM_OPEN_OPTIONS_INIT;
  pdcm_version_info_t version = PDCM_VERSION_INFO_INIT;
  pdcm_entity_filter_t filter = PDCM_ENTITY_FILTER_INIT;
  pdcm_entity_info_t info = PDCM_ENTITY_INFO_INIT;
  pdcm_capability_item_t item = PDCM_CAPABILITY_ITEM_INIT;
  pdcm_capability_set_t capabilities = PDCM_CAPABILITY_SET_INIT;
  pdcm_health_request_t health_request = PDCM_HEALTH_REQUEST_INIT;
  pdcm_health_result_t health = PDCM_HEALTH_RESULT_INIT;
  capabilities.items = &item;
  capabilities.item_capacity = 1u;
  pdcm_entity_ref_t entity = {
      .header = {.struct_size = (uint32_t)sizeof(pdcm_entity_ref_t),
                 .version = PDCM_STRUCT_VERSION_1},
      .kind = (uint32_t)PDCM_ENTITY_KIND_DEVICE,
      .reserved = 0u,
      .pdcm_id = 0u,
      .generation = 1u,
  };
  return entity.generation == 1u &&
                 options.mode == (uint32_t)PDCM_MODE_STANDALONE &&
                 version.header.struct_size == sizeof(pdcm_version_info_t) &&
                 filter.header.struct_size == sizeof(pdcm_entity_filter_t) &&
                 info.header.struct_size == sizeof(pdcm_entity_info_t) &&
                 health_request.subsystem_id ==
                     PDCM_HEALTH_SUBSYSTEM_FIRMWARE_HEARTBEAT &&
                 health.header.struct_size == sizeof(pdcm_health_result_t) &&
                 capabilities.item_capacity == 1u
             ? 0
             : 1;
}
