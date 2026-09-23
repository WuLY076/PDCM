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

# Production targets are added here as the implementation grows. This file is
# deliberately self-contained so product/ can be exported as a standalone
# repository without the GitHub-only testkit.
