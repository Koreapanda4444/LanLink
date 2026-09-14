cmake_minimum_required(VERSION 3.24)

project(LanLink VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

function(lanlink_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(lanlink_core STATIC
    src/core/config.cpp
    src/core/component.cpp
    src/core/logger.cpp
)

target_include_directories(lanlink_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(lanlink_core
    PRIVATE
        LANLINK_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(lanlink_core PUBLIC Threads::Threads)

lanlink_enable_warnings(lanlink_core)

add_library(lanlink_protocol STATIC
    src/protocol/codec.cpp
)

target_include_directories(lanlink_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

lanlink_enable_warnings(lanlink_protocol)

add_executable(lanlink_relay
    apps/relay/main.cpp
)

target_link_libraries(lanlink_relay PRIVATE lanlink_core)
set_target_properties(lanlink_relay PROPERTIES OUTPUT_NAME "lanlink-relay")
lanlink_enable_warnings(lanlink_relay)

if(WIN32)
    add_executable(lanlink_service
        apps/service/main.cpp
    )

    target_link_libraries(lanlink_service PRIVATE lanlink_core)
    set_target_properties(lanlink_service PROPERTIES OUTPUT_NAME "lanlink-service")
    lanlink_enable_warnings(lanlink_service)

    add_executable(lanlink_ui
        apps/ui/main.cpp
    )

    target_link_libraries(lanlink_ui PRIVATE lanlink_core)
    set_target_properties(lanlink_ui PROPERTIES OUTPUT_NAME "lanlink-ui")
    lanlink_enable_warnings(lanlink_ui)
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(lanlink_core_tests
        tests/core_tests.cpp
    )

    target_link_libraries(lanlink_core_tests PRIVATE lanlink_core)
    lanlink_enable_warnings(lanlink_core_tests)
    add_test(NAME lanlink-core COMMAND lanlink_core_tests)

    add_executable(lanlink_protocol_tests
        tests/protocol_tests.cpp
    )

    target_link_libraries(lanlink_protocol_tests PRIVATE lanlink_protocol)
    lanlink_enable_warnings(lanlink_protocol_tests)
    add_test(NAME lanlink-protocol COMMAND lanlink_protocol_tests)
endif()
