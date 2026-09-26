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
  find_package(GTest 1.14 CONFIG REQUIRED)
  include(GoogleTest)

  add_executable(
    pdcm_mock_provider_test
    tests/unit/mock_provider_test.cpp
  )
  target_link_libraries(
    pdcm_mock_provider_test
    PRIVATE pdcm_mock_provider GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_mock_provider_test)
  pdcm_enable_sanitizers(pdcm_mock_provider_test)
  gtest_discover_tests(
    pdcm_mock_provider_test
    TEST_PREFIX "testkit.mock_provider."
    PROPERTIES LABELS "tdd-10,ac-2,ac-3,ac-4,ac-8,mock-contract"
  )

  add_executable(
    pdcm_data_roundtrip_test
    tests/component/data_roundtrip_test.cpp
  )
  target_link_libraries(
    pdcm_data_roundtrip_test
    PRIVATE pdcm_mock_provider PDCM::data GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_data_roundtrip_test)
  pdcm_enable_sanitizers(pdcm_data_roundtrip_test)
  gtest_discover_tests(
    pdcm_data_roundtrip_test
    TEST_PREFIX "testkit.data_roundtrip."
    PROPERTIES LABELS "tdd-10,ac-3,ac-7,ac-8,mock-contract"
  )

  add_executable(
    pdcm_collection_roundtrip_test
    tests/component/collection_roundtrip_test.cpp
  )
  target_link_libraries(
    pdcm_collection_roundtrip_test
    PRIVATE pdcm_mock_provider PDCM::collection GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_collection_roundtrip_test)
  pdcm_enable_sanitizers(pdcm_collection_roundtrip_test)
  gtest_discover_tests(
    pdcm_collection_roundtrip_test
    TEST_PREFIX "testkit.collection_roundtrip."
    PROPERTIES LABELS "tdd-10,ac-3,ac-7,ac-8,mock-contract"
  )

  add_executable(
    pdcm_health_roundtrip_test
    tests/component/health_roundtrip_test.cpp
  )
  target_link_libraries(
    pdcm_health_roundtrip_test
    PRIVATE pdcm_mock_provider PDCM::collection PDCM::metrics GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_health_roundtrip_test)
  pdcm_enable_sanitizers(pdcm_health_roundtrip_test)
  gtest_discover_tests(
    pdcm_health_roundtrip_test
    TEST_PREFIX "testkit.health_roundtrip."
    PROPERTIES LABELS "tdd-10,ac-4,ac-7,ac-8,mock-contract"
  )

  add_executable(
    pdcm_core_lifecycle_test
    tests/component/core_lifecycle_test.cpp
  )
  target_link_libraries(
    pdcm_core_lifecycle_test
    PRIVATE pdcm_mock_provider PDCM::core GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_core_lifecycle_test)
  pdcm_enable_sanitizers(pdcm_core_lifecycle_test)
  gtest_discover_tests(
    pdcm_core_lifecycle_test
    TEST_PREFIX "testkit.core_lifecycle."
    PROPERTIES LABELS "tdd-10,ac-1,ac-2,ac-8,ac-9,mock-contract"
  )

  add_executable(
    pdcm_embedded_backend_test
    tests/component/embedded_backend_test.cpp
  )
  target_link_libraries(
    pdcm_embedded_backend_test
    PRIVATE pdcm_mock_provider PDCM::client_backend GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_embedded_backend_test)
  pdcm_enable_sanitizers(pdcm_embedded_backend_test)
  gtest_discover_tests(
    pdcm_embedded_backend_test
    TEST_PREFIX "testkit.embedded_backend."
    PROPERTIES LABELS "tdd-10,ac-1,ac-2,ac-8,mock-contract"
  )

  add_executable(
    pdcm_mock_daemon_fixture
    fixtures/mock_daemon_main.cpp
  )
  target_link_libraries(
    pdcm_mock_daemon_fixture
    PRIVATE pdcm_mock_provider PDCM::ipc
  )
  pdcm_enable_warnings(pdcm_mock_daemon_fixture)
  pdcm_enable_sanitizers(pdcm_mock_daemon_fixture)

  add_executable(
    pdcm_standalone_roundtrip_test
    tests/e2e/standalone_roundtrip_test.cpp
  )
  add_dependencies(
    pdcm_standalone_roundtrip_test
    pdcm_mock_daemon_fixture
  )
  target_compile_definitions(
    pdcm_standalone_roundtrip_test
    PRIVATE
      PDCM_MOCK_DAEMON_PATH="$<TARGET_FILE:pdcm_mock_daemon_fixture>"
  )
  target_link_libraries(
    pdcm_standalone_roundtrip_test
    PRIVATE PDCM::pdcm GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_standalone_roundtrip_test)
  pdcm_enable_sanitizers(pdcm_standalone_roundtrip_test)
  gtest_discover_tests(
    pdcm_standalone_roundtrip_test
    TEST_PREFIX "testkit.standalone_roundtrip."
    PROPERTIES LABELS "tdd-10,ac-1,ac-2,ac-8,rel-acc-03,mock-contract"
  )

  add_executable(
    pdcm_cli_roundtrip_test
    tests/e2e/cli_roundtrip_test.cpp
  )
  add_dependencies(
    pdcm_cli_roundtrip_test
    pdcm_mock_daemon_fixture
    pdcm_cli
  )
  target_compile_definitions(
    pdcm_cli_roundtrip_test
    PRIVATE
      PDCM_MOCK_DAEMON_PATH="$<TARGET_FILE:pdcm_mock_daemon_fixture>"
      PDCM_CLI_PATH="$<TARGET_FILE:pdcm_cli>"
  )
  target_link_libraries(
    pdcm_cli_roundtrip_test
    PRIVATE GTest::gtest_main
  )
  pdcm_enable_warnings(pdcm_cli_roundtrip_test)
  pdcm_enable_sanitizers(pdcm_cli_roundtrip_test)
  gtest_discover_tests(
    pdcm_cli_roundtrip_test
    TEST_PREFIX "testkit.cli_roundtrip."
    PROPERTIES LABELS "tdd-10,ac-1,ac-4,ac-8,rel-acc-03,mock-contract"
  )
endif()
