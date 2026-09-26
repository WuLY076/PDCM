add_library(
  pdcm_semantic
  STATIC
    semantic_catalog.cpp
)
add_library(PDCM::semantic ALIAS pdcm_semantic)
target_include_directories(
  pdcm_semantic
  PUBLIC
    "$<BUILD_INTERFACE:${PDCM_PRODUCT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_semantic
  PUBLIC pdcm_common pdcm_provider_contract
)
pdcm_enable_warnings(pdcm_semantic)
pdcm_enable_sanitizers(pdcm_semantic)
