if(NOT DEFINED PDCM_BUILD_DIR OR NOT DEFINED PDCM_INSTALL_LIBDIR OR
   NOT DEFINED PDCM_READELF)
  message(FATAL_ERROR "install-tree test arguments are required")
endif()

set(stage "${PDCM_BUILD_DIR}/install-tree-test")
file(REMOVE_RECURSE "${stage}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "DESTDIR=${stage}"
          "${CMAKE_COMMAND}" --install "${PDCM_BUILD_DIR}" --prefix /usr
  RESULT_VARIABLE install_status
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
if(NOT install_status EQUAL 0)
  message(FATAL_ERROR
          "staged install failed: ${install_output}\n${install_error}")
endif()

set(prefix "${stage}/usr")
set(required_files
    "bin/pdcm-cli"
    "sbin/pdcm-daemon"
    "include/pdcm/base.h"
    "include/pdcm/pdcm.h"
    "include/pdcm/status.h"
    "include/pdcm/types.h"
    "${PDCM_INSTALL_LIBDIR}/libpdcm.so"
    "${PDCM_INSTALL_LIBDIR}/pkgconfig/pdcm.pc"
    "${PDCM_INSTALL_LIBDIR}/cmake/PDCM/PDCMConfig.cmake"
    "${PDCM_INSTALL_LIBDIR}/cmake/PDCM/PDCMConfigVersion.cmake"
    "${PDCM_INSTALL_LIBDIR}/cmake/PDCM/PDCMTargets.cmake"
    "${PDCM_INSTALL_LIBDIR}/systemd/system/pdcm.service"
    "share/pdcm/catalog/fpga.yaml"
    "share/pdcm/catalog/emu.yaml"
    "share/doc/pdcm/INSTALL.md"
    "share/doc/pdcm/CLI_REFERENCE.md"
    "share/doc/pdcm/SINGLE_CARD_SUPPORT_MATRIX.md"
    "share/doc/pdcm/P0_METRIC_CATALOG.md"
    "share/doc/pdcm/FIRMWARE_HEARTBEAT_HEALTH_CONTRACT.md"
    "share/doc/pdcm/PUBLIC_API_REFERENCE.md"
    "share/doc/pdcm/ERROR_CODES.md"
    "share/doc/pdcm/TROUBLESHOOTING.md"
    "share/doc/pdcm/COMPATIBILITY_MATRIX.md"
    "share/doc/pdcm/KNOWN_ISSUES.md"
    "share/doc/pdcm/RELEASE_NOTES.md")
foreach(relative IN LISTS required_files)
  if(NOT EXISTS "${prefix}/${relative}")
    message(FATAL_ERROR "required install artifact is missing: ${relative}")
  endif()
endforeach()
if(NOT EXISTS "${stage}/etc/pdcm/pdcm.conf")
  message(FATAL_ERROR "runtime configuration was not installed")
endif()

file(GLOB_RECURSE installed_entries
     LIST_DIRECTORIES FALSE
     RELATIVE "${stage}"
     "${stage}/*")
foreach(relative IN LISTS installed_entries)
  if(relative MATCHES "(^|/)(testkit|mock|pdcm-diag-runner|libpdrl)(/|\\.|$)" OR
     relative MATCHES "\\.a$")
    message(FATAL_ERROR "forbidden install artifact: ${relative}")
  endif()
endforeach()

set(runtime_artifacts
    "${prefix}/${PDCM_INSTALL_LIBDIR}/libpdcm.so"
    "${prefix}/bin/pdcm-cli"
    "${prefix}/sbin/pdcm-daemon")
foreach(artifact IN LISTS runtime_artifacts)
  execute_process(
    COMMAND "${PDCM_READELF}" -d "${artifact}"
    RESULT_VARIABLE readelf_status
    OUTPUT_VARIABLE dynamic_section
    ERROR_VARIABLE readelf_error
  )
  if(NOT readelf_status EQUAL 0)
    message(FATAL_ERROR "readelf failed for ${artifact}: ${readelf_error}")
  endif()
  if(dynamic_section MATCHES "[Ll][Ii][Bb][Pp][Dd][Rr][Ll]")
    message(FATAL_ERROR "${artifact} has a forbidden libpdrl dependency")
  endif()
endforeach()

find_program(pkg_config_program pkg-config REQUIRED)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
          "PKG_CONFIG_PATH=${prefix}/${PDCM_INSTALL_LIBDIR}/pkgconfig"
          "${pkg_config_program}" --exists pdcm
  RESULT_VARIABLE pkg_config_status
)
if(NOT pkg_config_status EQUAL 0)
  message(FATAL_ERROR "installed pdcm.pc is not consumable")
endif()

set(consumer_source "${stage}/consumer")
file(MAKE_DIRECTORY "${consumer_source}")
file(WRITE "${consumer_source}/CMakeLists.txt"
     "cmake_minimum_required(VERSION 3.24)\n"
     "project(PdcmConsumer LANGUAGES C)\n"
     "find_package(PDCM CONFIG REQUIRED)\n"
     "add_library(consumer OBJECT consumer.c)\n"
     "target_link_libraries(consumer PRIVATE PDCM::pdcm)\n")
file(WRITE "${consumer_source}/consumer.c"
     "#include <pdcm/pdcm.h>\nint consumer(void) { return PDCM_STATUS_SUCCESS; }\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${consumer_source}"
          -B "${consumer_source}/build"
          "-DCMAKE_PREFIX_PATH=${prefix}"
  RESULT_VARIABLE consumer_status
  ERROR_VARIABLE consumer_error
)
if(NOT consumer_status EQUAL 0)
  message(FATAL_ERROR "installed CMake package is not consumable: ${consumer_error}")
endif()

file(READ "${prefix}/share/pdcm/catalog/fpga.yaml" fpga_catalog)
file(READ "${prefix}/share/pdcm/catalog/emu.yaml" emu_catalog)
foreach(catalog IN ITEMS fpga_catalog emu_catalog)
  if(NOT "${${catalog}}" MATCHES "metrics_status: BLOCKED_EXTERNAL")
    message(FATAL_ERROR "${catalog} must remain BLOCKED_EXTERNAL")
  endif()
  if("${${catalog}}" MATCHES "metrics:[ \t\r\n]+-[ \t]")
    message(FATAL_ERROR "${catalog} must not declare production metrics")
  endif()
endforeach()
