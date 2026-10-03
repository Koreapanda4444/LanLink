option(LANLINK_PACKAGE_RELAY "Create a native Linux relay distribution" OFF)
set(LANLINK_RELAY_PACKAGE_PLATFORM "linux" CACHE STRING "Relay distribution build platform")

if(LANLINK_PACKAGE_RELAY)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The relay service package requires Linux")
    endif()
    set_target_properties(lanlink_relay PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
    install(TARGETS lanlink_relay
        RUNTIME_DEPENDENCY_SET lanlink_relay_dependencies
        RUNTIME DESTINATION bin COMPONENT relay)
    install(RUNTIME_DEPENDENCY_SET lanlink_relay_dependencies
        DIRECTORIES "$<TARGET_FILE_DIR:${LANLINK_MSQUIC_TARGET}>"
        PRE_EXCLUDE_REGEXES "^ld-linux.*" "^lib(c|dl|m|pthread|rt)\\.so.*"
        POST_EXCLUDE_REGEXES "^/lib/.*" "^/lib64/.*" "^/usr/lib/.*" "^/usr/lib64/.*"
        LIBRARY DESTINATION lib COMPONENT relay)
    install(PROGRAMS
        "${CMAKE_CURRENT_SOURCE_DIR}/deploy/oracle-linux/install-relay.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}/deploy/oracle-linux/uninstall-relay.sh"
        DESTINATION . COMPONENT relay)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/deploy/oracle-linux/lanlink-relay.service"
        DESTINATION . COMPONENT relay)
    if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
        foreach(dependency msquic openssl sqlite3)
            install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${dependency}/copyright"
                DESTINATION licenses RENAME "${dependency}.txt" COMPONENT relay)
        endforeach()
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/relay-version.txt" "${PROJECT_VERSION}\n")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/relay-version.txt"
        DESTINATION . COMPONENT relay)
    set(CPACK_GENERATOR TGZ)
    set(CPACK_PACKAGE_NAME LanLink-relay)
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME "LanLink-relay-${PROJECT_VERSION}-${LANLINK_RELAY_PACKAGE_PLATFORM}-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_COMPONENTS_ALL relay)
    set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
    set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
    include(CPack)
endif()
