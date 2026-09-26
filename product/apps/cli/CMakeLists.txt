add_library(pdcm_cli_core STATIC cli.cpp)
target_include_directories(
  pdcm_cli_core
  PUBLIC "$<BUILD_INTERFACE:${PDCM_PRODUCT_SOURCE_DIR}/apps>"
)
target_link_libraries(pdcm_cli_core PUBLIC PDCM::pdcm)
pdcm_enable_warnings(pdcm_cli_core)
pdcm_enable_sanitizers(pdcm_cli_core)

add_executable(pdcm_cli main.cpp)
set_target_properties(pdcm_cli PROPERTIES OUTPUT_NAME pdcm-cli)
target_link_libraries(pdcm_cli PRIVATE pdcm_cli_core)
pdcm_enable_warnings(pdcm_cli)
pdcm_enable_sanitizers(pdcm_cli)
