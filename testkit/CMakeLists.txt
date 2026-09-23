# This directory is intentionally outside product/. Nothing defined here may
# be required to configure or build the production source tree.

add_library(pdcm_test_support INTERFACE)
target_include_directories(
  pdcm_test_support
  INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(
  pdcm_test_support
  INTERFACE PDCM::common PDCM::provider_contract
)

add_library(
  pdcm_mock_provider
  STATIC
    src/mock_provider.cpp
)
target_link_libraries(
  pdcm_mock_provider
  PUBLIC pdcm_test_support
)
pdcm_enable_warnings(pdcm_mock_provider)
pdcm_enable_sanitizers(pdcm_mock_provider)

if(BUILD_TESTING)
  add_executable(
    pdcm_mock_provider_test
    tests/mock_provider_test.cpp
  )
  target_link_libraries(
    pdcm_mock_provider_test
    PRIVATE pdcm_mock_provider
  )
  pdcm_enable_warnings(pdcm_mock_provider_test)
  pdcm_enable_sanitizers(pdcm_mock_provider_test)
  add_test(NAME testkit.mock_provider COMMAND pdcm_mock_provider_test)
endif()
