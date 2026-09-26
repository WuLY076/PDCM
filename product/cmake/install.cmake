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
  DIRECTORY "${PDCM_PRODUCT_SOURCE_DIR}/include/pdcm"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.h"
)

install(
  FILES "${PDCM_PRODUCT_SOURCE_DIR}/config/pdcm.conf"
  DESTINATION "/etc/pdcm"
  PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ
)
install(
  FILES "${PDCM_PRODUCT_SOURCE_DIR}/config/systemd/pdcm.service"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/system"
)

install(
  DIRECTORY "${PDCM_PRODUCT_SOURCE_DIR}/catalog/targets/"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/pdcm/catalog"
  FILES_MATCHING PATTERN "*.yaml"
)
install(
  DIRECTORY "${PDCM_PRODUCT_SOURCE_DIR}/docs/release/"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/pdcm"
  FILES_MATCHING PATTERN "*.md"
)

configure_package_config_file(
  "${PDCM_PRODUCT_SOURCE_DIR}/cmake/package/PDCMConfig.cmake.in"
  "${PDCM_PRODUCT_BINARY_DIR}/PDCMConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PDCM"
)
write_basic_package_version_file(
  "${PDCM_PRODUCT_BINARY_DIR}/PDCMConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)
configure_file(
  "${PDCM_PRODUCT_SOURCE_DIR}/cmake/package/pdcm.pc.in"
  "${PDCM_PRODUCT_BINARY_DIR}/pdcm.pc"
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
    "${PDCM_PRODUCT_BINARY_DIR}/PDCMConfig.cmake"
    "${PDCM_PRODUCT_BINARY_DIR}/PDCMConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PDCM"
)
install(
  FILES "${PDCM_PRODUCT_BINARY_DIR}/pdcm.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)
