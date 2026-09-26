cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(
    PDCM
    VERSION 0.1.0
    DESCRIPTION "PDCM production source tree"
    LANGUAGES C CXX
  )
  include(CTest)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PDCM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(PDCM_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PdcmWarnings)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
find_package(Protobuf 3.21 REQUIRED)

add_library(
  pdcm_common
  STATIC
    src/common/clock.cpp
    src/common/health.cpp
    src/common/observation.cpp
    src/common/status.cpp
)
add_library(PDCM::common ALIAS pdcm_common)

set_target_properties(pdcm_common PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(
  pdcm_common
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(pdcm_common PUBLIC cxx_std_17)
pdcm_enable_warnings(pdcm_common)
pdcm_enable_sanitizers(pdcm_common)

add_library(pdcm_provider_contract INTERFACE)
add_library(PDCM::provider_contract ALIAS pdcm_provider_contract)
target_include_directories(
  pdcm_provider_contract
  INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(pdcm_provider_contract INTERFACE pdcm_common)

add_library(
  pdcm_provider_runtime
  STATIC
    src/provider/provider_manager.cpp
)
add_library(PDCM::provider_runtime ALIAS pdcm_provider_runtime)
target_include_directories(
  pdcm_provider_runtime
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_provider_runtime
  PUBLIC pdcm_provider_contract
)
pdcm_enable_warnings(pdcm_provider_runtime)
pdcm_enable_sanitizers(pdcm_provider_runtime)

add_library(
  pdcm_semantic
  STATIC
    src/semantic/semantic_catalog.cpp
)
add_library(PDCM::semantic ALIAS pdcm_semantic)
target_include_directories(
  pdcm_semantic
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_semantic
  PUBLIC pdcm_common pdcm_provider_contract
)
pdcm_enable_warnings(pdcm_semantic)
pdcm_enable_sanitizers(pdcm_semantic)

add_library(
  pdcm_data
  STATIC
    src/data/data_manager.cpp
    src/data/event_store.cpp
    src/data/query_engine.cpp
    src/data/subscription_manager.cpp
)
add_library(PDCM::data ALIAS pdcm_data)
target_include_directories(
  pdcm_data
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_data
  PUBLIC pdcm_common pdcm_semantic
)
pdcm_enable_warnings(pdcm_data)
pdcm_enable_sanitizers(pdcm_data)

add_library(
  pdcm_metrics
  STATIC
    src/metrics/counter_processor.cpp
    src/metrics/metrics_manager.cpp
    src/metrics/processor.cpp
)
add_library(PDCM::metrics ALIAS pdcm_metrics)
target_include_directories(
  pdcm_metrics
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_metrics
  PUBLIC pdcm_common pdcm_data pdcm_semantic
)
pdcm_enable_warnings(pdcm_metrics)
pdcm_enable_sanitizers(pdcm_metrics)

add_library(
  pdcm_collection
  STATIC
    src/collection/collection_coordinator.cpp
    src/collection/watch_manager.cpp
)
add_library(PDCM::collection ALIAS pdcm_collection)
target_include_directories(
  pdcm_collection
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_collection
  PUBLIC
    pdcm_common pdcm_provider_runtime pdcm_semantic pdcm_data pdcm_metrics
)
pdcm_enable_warnings(pdcm_collection)
pdcm_enable_sanitizers(pdcm_collection)

add_library(
  pdcm_core
  STATIC
    src/core/runtime_config.cpp
    src/core/session_manager.cpp
    src/core/service_core.cpp
)
add_library(PDCM::core ALIAS pdcm_core)
target_include_directories(
  pdcm_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_core
  PUBLIC pdcm_common pdcm_provider_runtime pdcm_semantic pdcm_metrics
)
pdcm_enable_warnings(pdcm_core)
pdcm_enable_sanitizers(pdcm_core)

add_library(
  pdcm_local_protocol_proto
  STATIC
    proto/local/pdcm_local.proto
)
add_library(PDCM::local_protocol_proto ALIAS pdcm_local_protocol_proto)
protobuf_generate(
  TARGET pdcm_local_protocol_proto
  LANGUAGE cpp
)
target_include_directories(
  pdcm_local_protocol_proto
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>"
)
target_link_libraries(
  pdcm_local_protocol_proto
  PUBLIC protobuf::libprotobuf
)

add_library(
  pdcm_ipc
  STATIC
    src/ipc/frame.cpp
    src/ipc/handshake.cpp
    src/ipc/local_api_server.cpp
    src/ipc/request_router.cpp
    src/ipc/unix_socket.cpp
)
add_library(PDCM::ipc ALIAS pdcm_ipc)
target_include_directories(
  pdcm_ipc
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_ipc
  PUBLIC pdcm_common pdcm_core pdcm_local_protocol_proto
)
pdcm_enable_warnings(pdcm_ipc)
pdcm_enable_sanitizers(pdcm_ipc)

add_library(
  pdcm_client_backend
  STATIC
    src/client/embedded_backend.cpp
    src/client/standalone_backend.cpp
    src/provider/unavailable_provider.cpp
)
add_library(PDCM::client_backend ALIAS pdcm_client_backend)
target_include_directories(
  pdcm_client_backend
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
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
    src/client/pdcm_api.cpp
)
add_library(PDCM::pdcm ALIAS pdcm)
target_compile_definitions(pdcm PRIVATE PDCM_BUILD_SHARED)
target_include_directories(
  pdcm
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
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
      "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/libpdcm.map"
  )
  set_property(
    TARGET pdcm
    APPEND PROPERTY LINK_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libpdcm.map"
  )
endif()
pdcm_enable_warnings(pdcm)
pdcm_enable_sanitizers(pdcm)

add_library(pdcm_cli_core STATIC apps/cli/cli.cpp)
target_include_directories(
  pdcm_cli_core
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/apps>"
)
target_link_libraries(pdcm_cli_core PUBLIC PDCM::pdcm)
pdcm_enable_warnings(pdcm_cli_core)
pdcm_enable_sanitizers(pdcm_cli_core)

add_executable(pdcm_cli apps/cli/main.cpp)
set_target_properties(pdcm_cli PROPERTIES OUTPUT_NAME pdcm-cli)
target_link_libraries(pdcm_cli PRIVATE pdcm_cli_core)
pdcm_enable_warnings(pdcm_cli)
pdcm_enable_sanitizers(pdcm_cli)

add_library(pdcm_daemon_config STATIC apps/daemon/daemon_config.cpp)
target_include_directories(
  pdcm_daemon_config
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/apps>"
)
target_link_libraries(pdcm_daemon_config PUBLIC PDCM::core)
pdcm_enable_warnings(pdcm_daemon_config)
pdcm_enable_sanitizers(pdcm_daemon_config)

add_executable(pdcm_daemon apps/daemon/main.cpp)
set_target_properties(pdcm_daemon PROPERTIES OUTPUT_NAME pdcm-daemon)
target_link_libraries(
  pdcm_daemon
  PRIVATE pdcm_daemon_config PDCM::ipc pdcm_client_backend Threads::Threads
)
pdcm_enable_warnings(pdcm_daemon)
pdcm_enable_sanitizers(pdcm_daemon)

install(
  TARGETS pdcm
  EXPORT PDCMTargets
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(
  TARGETS pdcm_cli
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
  TARGETS pdcm_daemon
  RUNTIME DESTINATION "${CMAKE_INSTALL_SBINDIR}"
)

install(
  DIRECTORY include/pdcm
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.h"
)

install(
  FILES config/pdcm.conf
  DESTINATION "/etc/pdcm"
  PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ
)
install(
  FILES config/systemd/pdcm.service
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system"
)

install(
  DIRECTORY catalog/targets/
  DESTINATION "${CMAKE_INSTALL_DATADIR}/pdcm/catalog"
  FILES_MATCHING PATTERN "*.yaml"
)
install(
  DIRECTORY docs/release/
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/pdcm"
  FILES_MATCHING PATTERN "*.md"
)

configure_package_config_file(
  cmake/PDCMConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/PDCMConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PDCM"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/PDCMConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)
configure_file(
  cmake/pdcm.pc.in
  "${CMAKE_CURRENT_BINARY_DIR}/pdcm.pc"
  @ONLY
)
install(
  EXPORT PDCMTargets
  FILE PDCMTargets.cmake
  NAMESPACE PDCM::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PDCM"
)
install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PDCMConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PDCMConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PDCM"
)
install(
  FILES "${CMAKE_CURRENT_BINARY_DIR}/pdcm.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)

if(BUILD_TESTING)
  find_package(GTest 1.14 CONFIG REQUIRED)
  include(GoogleTest)
  find_program(PDCM_BASH bash REQUIRED)

  add_executable(pdcm_cli_test tests/unit/cli_test.cpp)
  add_test(NAME product.daemon.help COMMAND pdcm_daemon --help)
  add_test(
    NAME product.daemon.config_check
    COMMAND pdcm_daemon --config
            "${CMAKE_CURRENT_SOURCE_DIR}/config/pdcm.conf" --check-config
  )
  set_tests_properties(
    product.daemon.help
    PROPERTIES LABELS "tdd-09,ac-1,rel-acc-02"
  )
  set_tests_properties(product.daemon.config_check
                       PROPERTIES LABELS "tdd-01,ac-1,ac-8")

  add_executable(pdcm_daemon_config_test tests/unit/daemon_config_test.cpp)
  target_link_libraries(
    pdcm_daemon_config_test
    PRIVATE pdcm_daemon_config GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_daemon_config_test)
  pdcm_enable_sanitizers(pdcm_daemon_config_test)
  gtest_discover_tests(
    pdcm_daemon_config_test
    TEST_PREFIX "product.daemon_config."
    PROPERTIES LABELS "tdd-01,ac-1,ac-8"
  )

  add_executable(pdcm_daemon_lifecycle_test tests/unit/daemon_lifecycle_test.cpp)
  target_compile_definitions(
    pdcm_daemon_lifecycle_test
    PRIVATE PDCM_DAEMON_PATH="$<TARGET_FILE:pdcm_daemon>"
  )
  target_link_libraries(
    pdcm_daemon_lifecycle_test
    PRIVATE PDCM::pdcm GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_daemon_lifecycle_test)
  pdcm_enable_sanitizers(pdcm_daemon_lifecycle_test)
  gtest_discover_tests(
    pdcm_daemon_lifecycle_test
    TEST_PREFIX "product.daemon_lifecycle."
    PROPERTIES LABELS "tdd-01,tdd-10,ac-1,ac-8,ac-9,rel-acc-01,rel-acc-06"
  )

  target_link_libraries(
    pdcm_cli_test
    PRIVATE pdcm_cli_core GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_cli_test)
  pdcm_enable_sanitizers(pdcm_cli_test)
  gtest_discover_tests(
    pdcm_cli_test
    TEST_PREFIX "product.cli."
    PROPERTIES LABELS "tdd-09,tdd-10,ac-1,ac-5,ac-6,ac-10,ac-11,rel-acc-02,rel-acc-07"
  )

  add_executable(
    pdcm_public_headers_c_test
    tests/unit/public_headers_c_test.c
  )
  target_include_directories(
    pdcm_public_headers_c_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include"
  )
  pdcm_enable_warnings(pdcm_public_headers_c_test)
  add_test(NAME product.public_headers_c COMMAND pdcm_public_headers_c_test)
  set_tests_properties(product.public_headers_c
                       PROPERTIES LABELS "tdd-02,tdd-10,ac-1,api-abi-gate")

  add_executable(
    pdcm_public_api_test
    tests/unit/public_api_test.cpp
  )
  target_link_libraries(
    pdcm_public_api_test
    PRIVATE PDCM::pdcm GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_public_api_test)
  pdcm_enable_sanitizers(pdcm_public_api_test)
  gtest_discover_tests(
    pdcm_public_api_test
    TEST_PREFIX "product.public_api."
    PROPERTIES LABELS "tdd-02,tdd-10,ac-1,ac-5,rel-acc-04"
  )

  if(UNIX AND NOT APPLE)
    add_test(
      NAME product.public_symbols
      COMMAND
        ${CMAKE_COMMAND}
        "-DPDCM_NM=${CMAKE_NM}"
        "-DPDCM_LIBRARY=$<TARGET_FILE:pdcm>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_public_symbols.cmake"
    )
    set_tests_properties(
      product.public_symbols
      PROPERTIES LABELS "tdd-02,tdd-10,ac-1,ac-5,api-abi-gate"
    )
  endif()

  add_test(
    NAME product.install_tree
    COMMAND
      ${CMAKE_COMMAND}
      "-DPDCM_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPDCM_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
      "-DPDCM_READELF=${CMAKE_READELF}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_install_tree.cmake"
  )
  set_tests_properties(
    product.install_tree PROPERTIES RUN_SERIAL TRUE
  )
  set_property(
    TEST product.install_tree
    APPEND PROPERTY LABELS
      "tdd-09;tdd-10;ac-1;ac-5;ac-6;ac-10;ac-11;rel-acc-05;rel-acc-07;rel-acc-08"
  )

  add_test(
    NAME product.release_contract
    COMMAND "${PDCM_BASH}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify-release-contract.sh"
  )
  set_tests_properties(product.release_contract
                       PROPERTIES LABELS "tdd-10,ac-1,ac-2,ac-3,ac-4,ac-5,ac-6,ac-7,ac-8,ac-9,ac-10,ac-11")

  add_executable(
    pdcm_foundation_test
    tests/unit/foundation_test.cpp
  )
  target_include_directories(
    pdcm_foundation_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
  )
  target_link_libraries(
    pdcm_foundation_test
    PRIVATE pdcm_common pdcm_provider_contract GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_foundation_test)
  pdcm_enable_sanitizers(pdcm_foundation_test)
  gtest_discover_tests(
    pdcm_foundation_test
    TEST_PREFIX "product.foundation."
    PROPERTIES LABELS "tdd-06,tdd-10,ac-4,ac-8"
  )

  add_executable(
    pdcm_semantic_catalog_test
    tests/unit/semantic_catalog_test.cpp
  )
  target_link_libraries(
    pdcm_semantic_catalog_test
    PRIVATE pdcm_semantic GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_semantic_catalog_test)
  pdcm_enable_sanitizers(pdcm_semantic_catalog_test)
  gtest_discover_tests(
    pdcm_semantic_catalog_test
    TEST_PREFIX "product.semantic."
    PROPERTIES LABELS "tdd-03,tdd-10,ac-2,ac-3,ac-7,ac-8"
  )

  add_executable(
    pdcm_runtime_config_test
    tests/unit/runtime_config_test.cpp
  )
  target_link_libraries(
    pdcm_runtime_config_test
    PRIVATE pdcm_core GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_runtime_config_test)
  pdcm_enable_sanitizers(pdcm_runtime_config_test)
  gtest_discover_tests(
    pdcm_runtime_config_test
    TEST_PREFIX "product.runtime_config."
    PROPERTIES LABELS "tdd-01,tdd-10,ac-1,ac-8"
  )

  add_executable(
    pdcm_frame_test
    tests/unit/frame_test.cpp
  )
  target_link_libraries(
    pdcm_frame_test
    PRIVATE pdcm_ipc GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_frame_test)
  pdcm_enable_sanitizers(pdcm_frame_test)
  gtest_discover_tests(
    pdcm_frame_test
    TEST_PREFIX "product.ipc.frame."
    PROPERTIES LABELS "tdd-02,tdd-10,ac-1,ac-8,security-gate"
  )

  add_executable(
    pdcm_unix_socket_test
    tests/unit/unix_socket_test.cpp
  )
  target_link_libraries(
    pdcm_unix_socket_test
    PRIVATE pdcm_ipc GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_unix_socket_test)
  pdcm_enable_sanitizers(pdcm_unix_socket_test)
  gtest_discover_tests(
    pdcm_unix_socket_test
    TEST_PREFIX "product.ipc.unix_socket."
    PROPERTIES LABELS "tdd-02,tdd-10,ac-1,ac-8,security-gate"
  )

  add_executable(
    pdcm_session_handshake_test
    tests/unit/session_handshake_test.cpp
  )
  target_link_libraries(
    pdcm_session_handshake_test
    PRIVATE pdcm_ipc pdcm_core GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_session_handshake_test)
  pdcm_enable_sanitizers(pdcm_session_handshake_test)
  gtest_discover_tests(
    pdcm_session_handshake_test
    TEST_PREFIX "product.ipc.session_handshake."
    PROPERTIES LABELS "tdd-02,tdd-10,ac-1,ac-8,rel-acc-04,security-gate"
  )

  add_executable(
    pdcm_data_manager_test
    tests/unit/data_manager_test.cpp
  )
  target_link_libraries(
    pdcm_data_manager_test
    PRIVATE pdcm_data GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_data_manager_test)
  pdcm_enable_sanitizers(pdcm_data_manager_test)
  gtest_discover_tests(
    pdcm_data_manager_test
    TEST_PREFIX "product.data."
    PROPERTIES LABELS "tdd-05,tdd-10,ac-3,ac-4,ac-7,ac-8"
  )

  add_executable(
    pdcm_query_engine_test
    tests/unit/query_engine_test.cpp
  )
  target_link_libraries(
    pdcm_query_engine_test
    PRIVATE pdcm_data GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_query_engine_test)
  pdcm_enable_sanitizers(pdcm_query_engine_test)
  gtest_discover_tests(
    pdcm_query_engine_test
    TEST_PREFIX "product.query."
    PROPERTIES LABELS "tdd-05,tdd-10,ac-3,ac-4,ac-7,ac-8"
  )

  add_executable(
    pdcm_subscription_manager_test
    tests/unit/subscription_manager_test.cpp
  )
  target_link_libraries(
    pdcm_subscription_manager_test
    PRIVATE pdcm_data GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_subscription_manager_test)
  pdcm_enable_sanitizers(pdcm_subscription_manager_test)
  gtest_discover_tests(
    pdcm_subscription_manager_test
    TEST_PREFIX "product.subscription."
    PROPERTIES LABELS "tdd-05,tdd-10,ac-3,ac-4,ac-8,engineering-robustness"
  )

  add_executable(
    pdcm_processor_test
    tests/unit/processor_test.cpp
  )
  target_link_libraries(
    pdcm_processor_test
    PRIVATE pdcm_metrics GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_processor_test)
  pdcm_enable_sanitizers(pdcm_processor_test)
  gtest_discover_tests(
    pdcm_processor_test
    TEST_PREFIX "product.processor."
    PROPERTIES LABELS "tdd-06,tdd-10,ac-3,ac-7,ac-8"
  )

  add_executable(
    pdcm_metrics_manager_test
    tests/unit/metrics_manager_test.cpp
  )
  target_link_libraries(
    pdcm_metrics_manager_test
    PRIVATE pdcm_metrics GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_metrics_manager_test)
  pdcm_enable_sanitizers(pdcm_metrics_manager_test)
  gtest_discover_tests(
    pdcm_metrics_manager_test
    TEST_PREFIX "product.metrics."
    PROPERTIES LABELS "tdd-06,tdd-10,ac-3,ac-4,ac-7,ac-8"
  )

  add_executable(
    pdcm_watch_manager_test
    tests/unit/watch_manager_test.cpp
  )
  target_link_libraries(
    pdcm_watch_manager_test
    PRIVATE pdcm_collection GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_watch_manager_test)
  pdcm_enable_sanitizers(pdcm_watch_manager_test)
  gtest_discover_tests(
    pdcm_watch_manager_test
    TEST_PREFIX "product.watch."
    PROPERTIES LABELS "tdd-04,tdd-10,ac-3,ac-4,ac-8,ac-9"
  )

  add_executable(
    pdcm_collection_coordinator_test
    tests/unit/collection_coordinator_test.cpp
  )
  target_link_libraries(
    pdcm_collection_coordinator_test
    PRIVATE pdcm_collection GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_collection_coordinator_test)
  pdcm_enable_sanitizers(pdcm_collection_coordinator_test)
  gtest_discover_tests(
    pdcm_collection_coordinator_test
    TEST_PREFIX "product.collection."
    PROPERTIES LABELS "tdd-04,tdd-10,ac-3,ac-4,ac-7,ac-8,ac-9,engineering-robustness"
  )
endif()
