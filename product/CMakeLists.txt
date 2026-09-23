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

option(PDCM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(PDCM_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PdcmWarnings)
include(GNUInstallDirs)

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
endif()
