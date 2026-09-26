add_library(
  pdcm_core
  STATIC
    runtime_config.cpp
    session_manager.cpp
    service_core.cpp
)
add_library(PDCM::core ALIAS pdcm_core)
target_include_directories(
  pdcm_core
  PUBLIC
    "$<BUILD_INTERFACE:${PDCM_PRODUCT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_core
  PUBLIC pdcm_common pdcm_provider_runtime pdcm_semantic pdcm_metrics
)
pdcm_enable_warnings(pdcm_core)
pdcm_enable_sanitizers(pdcm_core)
