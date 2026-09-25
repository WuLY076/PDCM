cmake_minimum_required(VERSION 3.24)

project(
  PDCMWorkspace
  VERSION 0.1.0
  DESCRIPTION "PDCM development workspace"
  LANGUAGES C CXX
)

include(CTest)

option(PDCM_BUILD_TESTKIT "Build GitHub-only testkit and MockProvider" ON)

add_subdirectory(product)

if(PDCM_BUILD_TESTKIT)
  add_subdirectory(testkit)
endif()
