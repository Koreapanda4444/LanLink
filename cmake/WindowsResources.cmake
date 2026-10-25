if(WIN32)
    enable_language(RC)

    function(lanlink_windows_resources target description)
        get_target_property(LANLINK_FILE_NAME ${target} OUTPUT_NAME)
        set(LANLINK_FILE_DESCRIPTION "${description}")
        set(resource "${CMAKE_CURRENT_BINARY_DIR}/resources/${target}.rc")
        configure_file("${PROJECT_SOURCE_DIR}/resources/windows/lanlink.rc.in"
            "${resource}" @ONLY)
        target_sources(${target} PRIVATE "${resource}")
        target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/resources/windows")
    endfunction()

    lanlink_windows_resources(lanlink_ui "LanLink desktop")
    lanlink_windows_resources(lanlink_service "LanLink Windows service")
    lanlink_windows_resources(lanlink_ui_cli "LanLink command-line control")
    lanlink_windows_resources(lanlink_relay "LanLink relay")
endif()
