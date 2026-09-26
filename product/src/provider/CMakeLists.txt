add_library(pdcm_provider_contract INTERFACE)
add_library(PDCM::provider_contract ALIAS pdcm_provider_contract)
target_include_directories(
  pdcm_provider_contract
  INTERFACE
    "$<BUILD_INTERFACE:${PDCM_PRODUCT_SOURCE_DIR}/src>"
)
target_link_libraries(pdcm_provider_contract INTERFACE pdcm_common)

add_library(
  pdcm_provider_runtime
  STATIC
    provider_manager.cpp
)
add_library(PDCM::provider_runtime ALIAS pdcm_provider_runtime)
target_include_directories(
  pdcm_provider_runtime
  PUBLIC
    "$<BUILD_INTERFACE:${PDCM_PRODUCT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_provider_runtime
  PUBLIC pdcm_provider_contract
)
pdcm_enable_warnings(pdcm_provider_runtime)
pdcm_enable_sanitizers(pdcm_provider_runtime)
