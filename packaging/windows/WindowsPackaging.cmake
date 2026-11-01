# Windows resources and installer staging (feature/windows-desktop only).
#
# 1. The executable gets an icon and version information from haul.rc.
# 2. `cmake --install <build> --prefix <stage>` puts haul.exe in <stage>/bin;
#    packaging/windows/stage.ps1 then adds the Qt runtime (windeployqt) and the
#    vcpkg DLLs, and installer.nsi turns the staged folder into Haul-<ver>-Setup.exe.

set(HAUL_ICO ${PROJECT_SOURCE_DIR}/packaging/windows/haul.ico)
configure_file(${PROJECT_SOURCE_DIR}/packaging/windows/haul.rc.in
               ${PROJECT_BINARY_DIR}/haul.rc @ONLY)
target_sources(haul PRIVATE ${PROJECT_BINARY_DIR}/haul.rc)

install(FILES ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
