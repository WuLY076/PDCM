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
option(PDCM_ENABLE_SANITIZERS
       "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(PDCM_PRODUCT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(PDCM_PRODUCT_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")

list(APPEND CMAKE_MODULE_PATH
     "${PDCM_PRODUCT_SOURCE_DIR}/cmake/modules")
include(PdcmWarnings)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

find_package(Protobuf 3.21 REQUIRED)
find_package(Threads REQUIRED)

add_subdirectory(src/common)
add_subdirectory(src/provider)
add_subdirectory(src/semantic)
add_subdirectory(src/data)
add_subdirectory(src/metrics)
add_subdirectory(src/collection)
add_subdirectory(src/core)
add_subdirectory(proto/local/v1)
add_subdirectory(src/ipc)
add_subdirectory(src/client)
add_subdirectory(apps/cli)
add_subdirectory(apps/daemon)

include(cmake/install.cmake)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
