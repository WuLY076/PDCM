add_library(
  pdcm_data
  STATIC
    data_manager.cpp
    event_store.cpp
    query_engine.cpp
    subscription_manager.cpp
)
add_library(PDCM::data ALIAS pdcm_data)
target_include_directories(
  pdcm_data
  PUBLIC
    "$<BUILD_INTERFACE:${PDCM_PRODUCT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_data
  PUBLIC pdcm_common pdcm_semantic
)
pdcm_enable_warnings(pdcm_data)
pdcm_enable_sanitizers(pdcm_data)
