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
include(GNUInstallDirs)
find_package(Protobuf 3.21 REQUIRED)

add_library(
  pdcm_common
  STATIC
    src/common/clock.cpp
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
  pdcm_core
  STATIC
    src/core/runtime_config.cpp
    src/core/session_manager.cpp
    src/core/service_core.cpp
    src/provider/provider_manager.cpp
)
add_library(PDCM::core ALIAS pdcm_core)
target_include_directories(
  pdcm_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(
  pdcm_core
  PUBLIC pdcm_common pdcm_provider_contract pdcm_semantic
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

install(
  TARGETS pdcm
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

install(
  DIRECTORY include/pdcm
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.h"
)

if(BUILD_TESTING)
  find_package(GTest 1.14 CONFIG REQUIRED)
  include(GoogleTest)

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
  endif()

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
  )
endif()
