# Linux desktop integration and Debian packaging (feature/linux-desktop only).
#
#   cmake --preset linux-release
#   cmake --build --preset linux-release
#   cd build/linux-release && cpack -G DEB
#
# Produces build/linux-release/haul_<version>_<arch>.deb

install(FILES ${PROJECT_SOURCE_DIR}/packaging/linux/haul.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${PROJECT_SOURCE_DIR}/assets/haul.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/linux/io.github.tehsaad.Haul.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

set(CPACK_PACKAGE_NAME "haul")
set(CPACK_PACKAGE_VENDOR "Haul contributors")
set(CPACK_PACKAGE_CONTACT "Saad <tehsaad13453310@gmail.com>")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Open-source download manager with resume, queues and multi-connection downloads")
set(CPACK_PACKAGE_HOMEPAGE_URL ${PROJECT_HOMEPAGE_URL})
set(CPACK_RESOURCE_FILE_LICENSE ${PROJECT_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
"Open-source desktop download manager
 Haul downloads files over HTTP and HTTPS with safe pause and resume,
 multi-connection (segmented) transfers, a priority queue, a daily scheduler,
 bandwidth limits, automatic retry and SHA-256 verification.")
# Shared libraries are detected automatically by dpkg-shlibdeps; Qt plugins are loaded at
# run time (SVG icons, platform integration), so they must be listed explicitly.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-svg-plugins, qt6-qpa-plugins")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "ffmpeg")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-wayland")

include(CPack)
