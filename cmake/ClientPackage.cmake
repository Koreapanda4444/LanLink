option(LANLINK_PACKAGE_WINDOWS_CLIENT "Create the Windows client distribution" OFF)
set(LANLINK_WINTUN_DLL "" CACHE FILEPATH "Signed x64 wintun.dll from the official distribution")
set(LANLINK_WINTUN_LICENSE_FILE "" CACHE FILEPATH "License accompanying the Wintun binary")

if(LANLINK_PACKAGE_WINDOWS_CLIENT)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The Windows client package requires a 64-bit Windows build")
    endif()
    foreach(required_file LANLINK_WINTUN_DLL LANLINK_WINTUN_LICENSE_FILE)
        if(NOT EXISTS "${${required_file}}")
            message(FATAL_ERROR "${required_file} must name an existing file")
        endif()
    endforeach()

    install(TARGETS lanlink_service lanlink_ui lanlink_ui_cli
        RUNTIME_DEPENDENCY_SET lanlink_client_dependencies
        RUNTIME DESTINATION bin COMPONENT client)
    install(RUNTIME_DEPENDENCY_SET lanlink_client_dependencies
        DIRECTORIES "$<TARGET_FILE_DIR:lanlink_service>"
        PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
        POST_EXCLUDE_REGEXES ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*"
        RUNTIME DESTINATION bin COMPONENT client)
    install(FILES "${LANLINK_WINTUN_DLL}" DESTINATION bin
        RENAME wintun.dll COMPONENT client)
    install(FILES "${LANLINK_WINTUN_LICENSE_FILE}" DESTINATION licenses
        RENAME Wintun.txt COMPONENT client)
    if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
        foreach(dependency msquic openssl sqlite3 imgui)
            install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${dependency}/copyright"
                DESTINATION licenses RENAME "${dependency}.txt" COMPONENT client)
        endforeach()
    endif()
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/Install-LanLink.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/Uninstall-LanLink.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/ClientSetup.psm1"
        DESTINATION . COMPONENT client)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/package-version.txt" "${PROJECT_VERSION}\n")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/package-version.txt"
        DESTINATION . COMPONENT client)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/build-info.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/release/INSTALL.md"
        DESTINATION . COMPONENT client)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/release/0.1.0.md"
        DESTINATION . RENAME RELEASE-NOTES.md COMPONENT client)

    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT client)
    include(InstallRequiredSystemLibraries)
    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGE_NAME LanLink)
    set(CPACK_PACKAGE_VENDOR Koreapanda4444)
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME "LanLink-${PROJECT_VERSION}-windows-x64")
    set(CPACK_COMPONENTS_ALL client)
    set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
    set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
    include(CPack)
endif()
