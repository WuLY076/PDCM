add_library(
  pdcm_metrics
  STATIC
    counter_processor.cpp
    metrics_manager.cpp
    processor.cpp
)
add_library(PDCM::metrics ALIAS pdcm_metrics)
target_include_directories(
  pdcm_metrics
  PUBLIC
    "$<BUILD_INTERFACE:${PDCM_PRODUCT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_metrics
  PUBLIC pdcm_common pdcm_data pdcm_semantic
)
pdcm_enable_warnings(pdcm_metrics)
pdcm_enable_sanitizers(pdcm_metrics)
