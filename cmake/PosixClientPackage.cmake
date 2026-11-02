if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LANLINK_BUILD_DESKTOP)
    set_target_properties(lanlink_service lanlink_ui lanlink_ui_cli PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
    install(TARGETS lanlink_service lanlink_ui lanlink_ui_cli
        RUNTIME_DEPENDENCY_SET lanlink_posix_client_dependencies
        RUNTIME DESTINATION bin COMPONENT portable-client)
    install(RUNTIME_DEPENDENCY_SET lanlink_posix_client_dependencies
        DIRECTORIES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
        POST_EXCLUDE_REGEXES "^/lib/.*" "^/lib64/.*" "^/usr/lib/.*"
        LIBRARY DESTINATION lib COMPONENT portable-client)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/branding/lanlink.png"
        DESTINATION share/lanlink COMPONENT portable-client)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/install-client.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/posix/client_setup.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/uninstall-client.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/lanlink.desktop"
        "${CMAKE_CURRENT_SOURCE_DIR}/deploy/linux/lanlink-client.service"
        "${CMAKE_CURRENT_BINARY_DIR}/build-info.json"
        DESTINATION . COMPONENT portable-client)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/clients-posix.md"
        DESTINATION . RENAME INSTALL.md COMPONENT portable-client)
    foreach(dependency msquic openssl sqlite3 imgui sdl3 libpng zlib)
        install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${dependency}/copyright"
            DESTINATION licenses RENAME "${dependency}.txt" COMPONENT portable-client)
    endforeach()
    add_custom_target(lanlink_client_archive
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/artifacts" "${CMAKE_CURRENT_BINARY_DIR}/portable-client/lib"
        COMMAND ${CMAKE_COMMAND} --install "${CMAKE_CURRENT_BINARY_DIR}" --prefix "${CMAKE_CURRENT_BINARY_DIR}/portable-client" --component portable-client
        COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_CURRENT_BINARY_DIR}/portable-client"
            ${CMAKE_COMMAND} -E tar czf "${CMAKE_CURRENT_BINARY_DIR}/artifacts/LanLink-${PROJECT_VERSION}-linux-${LANLINK_PACKAGE_ARCHITECTURE}.tar.gz"
            --format=gnutar bin lib share licenses build-info.json INSTALL.md client_setup.py install-client.py uninstall-client.py lanlink.desktop lanlink-client.service
        DEPENDS lanlink_service lanlink_ui lanlink_ui_cli)
endif()

if(APPLE AND LANLINK_BUILD_DESKTOP)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set_target_properties(lanlink_service lanlink_ui_cli PROPERTIES INSTALL_RPATH "@executable_path/../lib")
    set_target_properties(lanlink_ui PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
    install(TARGETS lanlink_service lanlink_ui_cli lanlink_ui
        RUNTIME_DEPENDENCY_SET lanlink_macos_dependencies
        RUNTIME DESTINATION bin COMPONENT portable-client BUNDLE DESTINATION . COMPONENT portable-client)
    install(RUNTIME_DEPENDENCY_SET lanlink_macos_dependencies
        DIRECTORIES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
        POST_EXCLUDE_REGEXES "^/System/Library/.*" "^/usr/lib/.*"
        LIBRARY DESTINATION lib COMPONENT portable-client)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/install-client.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/uninstall-client.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/posix/client_setup.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/deploy/macos/com.lanlink.client.plist"
        "${CMAKE_CURRENT_BINARY_DIR}/build-info.json" DESTINATION . COMPONENT portable-client)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/clients-posix.md"
        DESTINATION . RENAME INSTALL.md COMPONENT portable-client)
    foreach(dependency msquic openssl sqlite3 imgui sdl3 libpng zlib)
        install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${dependency}/copyright"
            DESTINATION licenses RENAME "${dependency}.txt" COMPONENT portable-client)
    endforeach()
    install(CODE "
        execute_process(COMMAND \"${Python3_EXECUTABLE}\" \"${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/relocate.py\" \"\${CMAKE_INSTALL_PREFIX}\" RESULT_VARIABLE relocated)
        if(NOT relocated EQUAL 0)
            message(FATAL_ERROR \"macOS library relocation or signature verification failed\")
        endif()" COMPONENT portable-client)
    add_custom_target(lanlink_client_archive
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/portable-client/lib"
        COMMAND ${CMAKE_COMMAND} --install "${CMAKE_CURRENT_BINARY_DIR}" --prefix "${CMAKE_CURRENT_BINARY_DIR}/portable-client" --component portable-client
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/make_archive.py"
            "${CMAKE_CURRENT_BINARY_DIR}/portable-client" "${CMAKE_CURRENT_BINARY_DIR}/artifacts"
        DEPENDS lanlink_service lanlink_ui lanlink_ui_cli)
endif()
