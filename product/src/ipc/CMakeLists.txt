add_library(
  pdcm_ipc
  STATIC
    frame.cpp
    handshake.cpp
    local_api_server.cpp
    request_router.cpp
    unix_socket.cpp
)
add_library(PDCM::ipc ALIAS pdcm_ipc)
target_include_directories(
  pdcm_ipc
  PUBLIC
    "$<BUILD_INTERFACE:${PDCM_PRODUCT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_ipc
  PUBLIC pdcm_common pdcm_core pdcm_local_protocol_proto
)
pdcm_enable_warnings(pdcm_ipc)
pdcm_enable_sanitizers(pdcm_ipc)
