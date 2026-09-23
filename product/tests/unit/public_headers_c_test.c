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
                 version.header.struct_size == sizeof(pdcm_version_info_t)
             ? 0
             : 1;
}
