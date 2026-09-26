add_library(
  pdcm_client_backend
  STATIC
    embedded_backend.cpp
    standalone_backend.cpp
    ${PDCM_PRODUCT_SOURCE_DIR}/src/provider/unavailable_provider.cpp
)
add_library(PDCM::client_backend ALIAS pdcm_client_backend)
target_include_directories(
  pdcm_client_backend
  PUBLIC
    "$<BUILD_INTERFACE:${PDCM_PRODUCT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_client_backend
  PUBLIC pdcm_core pdcm_ipc
)
pdcm_enable_warnings(pdcm_client_backend)
pdcm_enable_sanitizers(pdcm_client_backend)

add_library(
  pdcm
  SHARED
    pdcm_api.cpp
)
add_library(PDCM::pdcm ALIAS pdcm)
target_compile_definitions(pdcm PRIVATE PDCM_BUILD_SHARED)
target_include_directories(
  pdcm
  PUBLIC
    "$<BUILD_INTERFACE:${PDCM_PRODUCT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${PDCM_PRODUCT_SOURCE_DIR}/src"
)
target_link_libraries(
  pdcm
  PRIVATE pdcm_client_backend
)
set_target_properties(
  pdcm
  PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION 0
)
if(UNIX AND NOT APPLE)
  target_link_options(
    pdcm
    PRIVATE
      "LINKER:--version-script=${PDCM_PRODUCT_SOURCE_DIR}/cmake/abi/libpdcm.map"
  )
  set_property(
    TARGET pdcm
    APPEND PROPERTY LINK_DEPENDS
      "${PDCM_PRODUCT_SOURCE_DIR}/cmake/abi/libpdcm.map"
  )
endif()
pdcm_enable_warnings(pdcm)
pdcm_enable_sanitizers(pdcm)
