add_library(
  pdcm_common
  STATIC
    clock.cpp
    health.cpp
    observation.cpp
    status.cpp
)
add_library(PDCM::common ALIAS pdcm_common)

set_target_properties(pdcm_common PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(
  pdcm_common
  PUBLIC
    "$<BUILD_INTERFACE:${PDCM_PRODUCT_SOURCE_DIR}/include>"
  PRIVATE
    "${PDCM_PRODUCT_SOURCE_DIR}/src"
)
target_compile_features(pdcm_common PUBLIC cxx_std_17)
pdcm_enable_warnings(pdcm_common)
pdcm_enable_sanitizers(pdcm_common)
