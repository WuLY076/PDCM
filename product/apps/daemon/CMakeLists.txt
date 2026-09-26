add_library(pdcm_daemon_config STATIC daemon_config.cpp)
target_include_directories(
  pdcm_daemon_config
  PUBLIC "$<BUILD_INTERFACE:${PDCM_PRODUCT_SOURCE_DIR}/apps>"
)
target_link_libraries(pdcm_daemon_config PUBLIC PDCM::core)
pdcm_enable_warnings(pdcm_daemon_config)
pdcm_enable_sanitizers(pdcm_daemon_config)

add_executable(pdcm_daemon main.cpp)
set_target_properties(pdcm_daemon PROPERTIES OUTPUT_NAME pdcm-daemon)
target_link_libraries(
  pdcm_daemon
  PRIVATE pdcm_daemon_config PDCM::ipc pdcm_client_backend Threads::Threads
)
pdcm_enable_warnings(pdcm_daemon)
pdcm_enable_sanitizers(pdcm_daemon)
