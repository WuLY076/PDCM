  find_package(GTest 1.14 CONFIG REQUIRED)
  include(GoogleTest)
  find_program(PDCM_BASH bash REQUIRED)

  add_executable(pdcm_cli_test unit/cli/cli_test.cpp)
  add_test(NAME product.daemon.help COMMAND pdcm_daemon --help)
  add_test(
    NAME product.daemon.config_check
    COMMAND pdcm_daemon --config
            "${PDCM_PRODUCT_SOURCE_DIR}/config/pdcm.conf" --check-config
  )
  set_tests_properties(
    product.daemon.help
    PROPERTIES LABELS "tdd-09,ac-1,rel-acc-02"
  )
  set_tests_properties(product.daemon.config_check
                       PROPERTIES LABELS "tdd-01,ac-1,ac-8")

  add_executable(pdcm_daemon_config_test unit/daemon/daemon_config_test.cpp)
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

  add_executable(pdcm_daemon_lifecycle_test integration/daemon_lifecycle_test.cpp)
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
    contract/public_headers_c_test.c
  )
  target_include_directories(
    pdcm_public_headers_c_test
    PRIVATE "${PDCM_PRODUCT_SOURCE_DIR}/include"
  )
  pdcm_enable_warnings(pdcm_public_headers_c_test)
  add_test(NAME product.public_headers_c COMMAND pdcm_public_headers_c_test)
  set_tests_properties(product.public_headers_c
                       PROPERTIES LABELS "tdd-02,tdd-10,ac-1,api-abi-gate")

  add_executable(
    pdcm_public_api_test
    contract/public_api_test.cpp
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
        -P "${CMAKE_CURRENT_SOURCE_DIR}/contract/check_public_symbols.cmake"
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
      "-DPDCM_BUILD_DIR=${PDCM_PRODUCT_BINARY_DIR}"
      "-DPDCM_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
      "-DPDCM_READELF=${CMAKE_READELF}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/install/check_install_tree.cmake"
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
            "${PDCM_PRODUCT_SOURCE_DIR}/tools/release/verify-release-contract.sh"
  )
  set_tests_properties(product.release_contract
                       PROPERTIES LABELS "tdd-10,ac-1,ac-2,ac-3,ac-4,ac-5,ac-6,ac-7,ac-8,ac-9,ac-10,ac-11")

  add_executable(
    pdcm_foundation_test
    unit/common/foundation_test.cpp
  )
  target_include_directories(
    pdcm_foundation_test
    PRIVATE "${PDCM_PRODUCT_SOURCE_DIR}/src"
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
    unit/semantic/semantic_catalog_test.cpp
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
    unit/core/runtime_config_test.cpp
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
    unit/ipc/frame_test.cpp
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
    integration/unix_socket_test.cpp
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
    unit/ipc/session_handshake_test.cpp
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
    unit/data/data_manager_test.cpp
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
    unit/data/query_engine_test.cpp
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
    unit/data/subscription_manager_test.cpp
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
    unit/metrics/processor_test.cpp
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
    unit/metrics/metrics_manager_test.cpp
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
    unit/collection/watch_manager_test.cpp
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
    unit/collection/collection_coordinator_test.cpp
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
