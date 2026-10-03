cmake_minimum_required(VERSION 3.24)

project(LanLink VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(msquic CONFIG REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
find_package(SQLite3 REQUIRED)

if(TARGET SQLite3::SQLite3)
    set(LANLINK_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
    set(LANLINK_SQLITE_TARGET SQLite::SQLite3)
else()
    message(FATAL_ERROR "The SQLite3 package did not export a supported CMake target")
endif()

if(TARGET msquic::msquic)
    set(LANLINK_MSQUIC_TARGET msquic::msquic)
elseif(TARGET msquic)
    set(LANLINK_MSQUIC_TARGET msquic)
else()
    message(FATAL_ERROR "The msquic package did not export a supported CMake target")
endif()

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
    src/protocol/local_control.cpp
    src/protocol/network_messages.cpp
    src/protocol/packet_messages.cpp
    src/protocol/stream_decoder.cpp
)

target_include_directories(lanlink_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

lanlink_enable_warnings(lanlink_protocol)

add_library(lanlink_auth STATIC
    src/auth/device_keys.cpp
    src/auth/network_keys.cpp
    src/auth/packet_crypto.cpp
    src/auth/handshake.cpp
    src/auth/identity.cpp
    src/auth/secret_store.cpp
)

target_include_directories(lanlink_auth
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(lanlink_auth
    PUBLIC
        lanlink_protocol
    PRIVATE
        OpenSSL::Crypto
)

if(WIN32)
    target_link_libraries(lanlink_auth PRIVATE Crypt32)
endif()

lanlink_enable_warnings(lanlink_auth)

add_library(lanlink_storage STATIC
    src/storage/relay_store.cpp
)

target_include_directories(lanlink_storage
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(lanlink_storage PRIVATE ${LANLINK_SQLITE_TARGET})

lanlink_enable_warnings(lanlink_storage)

add_library(lanlink_relay_domain STATIC
    src/relay/network_control_channel.cpp
    src/relay/network_service.cpp
)

target_include_directories(lanlink_relay_domain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(lanlink_relay_domain
    PUBLIC
        lanlink_auth
        lanlink_protocol
        lanlink_storage
    PRIVATE
        OpenSSL::Crypto
)

lanlink_enable_warnings(lanlink_relay_domain)

add_library(lanlink_transport STATIC
    src/transport/packet_bridge.cpp
    src/transport/quic.cpp
    src/transport/reconnect.cpp
    src/transport/tls.cpp
)

target_include_directories(lanlink_transport
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(lanlink_transport
    PUBLIC
        lanlink_core
    PRIVATE
        lanlink_auth
        lanlink_protocol
        lanlink_relay_domain
        ${LANLINK_MSQUIC_TARGET}
        OpenSSL::SSL
)

if(WIN32)
    target_link_libraries(lanlink_transport PRIVATE ntdll ws2_32)

    target_sources(lanlink_transport PRIVATE src/transport/wintun_adapter.cpp)
    target_link_libraries(lanlink_transport PRIVATE iphlpapi)
endif()

lanlink_enable_warnings(lanlink_transport)

add_executable(lanlink_relay
    apps/relay/main.cpp
)

target_link_libraries(lanlink_relay PRIVATE lanlink_core lanlink_transport)
set_target_properties(lanlink_relay PROPERTIES OUTPUT_NAME "lanlink-relay")
lanlink_enable_warnings(lanlink_relay)

if(WIN32)
    find_package(imgui CONFIG REQUIRED)

    add_library(lanlink_local_ipc STATIC
        src/transport/local_pipe.cpp
    )
    target_include_directories(lanlink_local_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(lanlink_local_ipc PUBLIC lanlink_protocol PRIVATE advapi32)
    lanlink_enable_warnings(lanlink_local_ipc)

    add_executable(lanlink_service
        apps/service/main.cpp
        apps/service/windows_service.cpp
    )

    target_link_libraries(lanlink_service PRIVATE lanlink_core lanlink_transport lanlink_local_ipc advapi32)
    set_target_properties(lanlink_service PROPERTIES OUTPUT_NAME "lanlink-service")
    lanlink_enable_warnings(lanlink_service)

    add_executable(lanlink_ui WIN32
        apps/ui/main.cpp
        apps/ui/network_model.cpp
    )

    target_link_libraries(lanlink_ui PRIVATE lanlink_core lanlink_local_ipc imgui::imgui d3d11 dxgi)
    set_target_properties(lanlink_ui PROPERTIES OUTPUT_NAME "lanlink-ui")
    lanlink_enable_warnings(lanlink_ui)

    add_executable(lanlink_ui_cli apps/ui/control_cli.cpp)
    target_link_libraries(lanlink_ui_cli PRIVATE lanlink_core lanlink_local_ipc)
    set_target_properties(lanlink_ui_cli PROPERTIES OUTPUT_NAME "lanlink-ui-cli")
    lanlink_enable_warnings(lanlink_ui_cli)
endif()

include(CTest)
include(cmake/ClientPackage.cmake)
include(cmake/RelayPackage.cmake)

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

    add_executable(lanlink_local_control_tests
        tests/local_control_tests.cpp
    )
    target_link_libraries(lanlink_local_control_tests PRIVATE lanlink_protocol)
    lanlink_enable_warnings(lanlink_local_control_tests)
    add_test(NAME lanlink-local-control COMMAND lanlink_local_control_tests)

    if(WIN32)
        add_executable(lanlink_local_pipe_tests tests/local_pipe_tests.cpp)
        target_link_libraries(lanlink_local_pipe_tests PRIVATE lanlink_local_ipc)
        lanlink_enable_warnings(lanlink_local_pipe_tests)
        add_test(NAME lanlink-local-pipe COMMAND lanlink_local_pipe_tests)
        set_tests_properties(lanlink-local-pipe PROPERTIES
            RESOURCE_LOCK lanlink-service-pipe TIMEOUT 30)

        add_executable(lanlink_ui_network_model_tests
            tests/ui_network_model_tests.cpp
            apps/ui/network_model.cpp
        )
        target_link_libraries(lanlink_ui_network_model_tests PRIVATE lanlink_local_ipc)
        lanlink_enable_warnings(lanlink_ui_network_model_tests)
        add_test(NAME lanlink-ui-network-model COMMAND lanlink_ui_network_model_tests)
        set_tests_properties(lanlink-ui-network-model PROPERTIES
            RESOURCE_LOCK lanlink-service-pipe TIMEOUT 30)
    endif()

    add_executable(lanlink_network_protocol_tests
        tests/network_protocol_tests.cpp
    )

    target_link_libraries(lanlink_network_protocol_tests PRIVATE lanlink_protocol)
    lanlink_enable_warnings(lanlink_network_protocol_tests)
    add_test(NAME lanlink-network-protocol COMMAND lanlink_network_protocol_tests)

    add_executable(lanlink_auth_tests
        tests/auth_tests.cpp
    )

    target_link_libraries(lanlink_auth_tests PRIVATE lanlink_auth)
    lanlink_enable_warnings(lanlink_auth_tests)
    add_test(NAME lanlink-auth COMMAND lanlink_auth_tests)

    add_executable(lanlink_packet_crypto_tests
        tests/packet_crypto_tests.cpp
    )

    target_link_libraries(lanlink_packet_crypto_tests PRIVATE lanlink_auth)
    lanlink_enable_warnings(lanlink_packet_crypto_tests)
    add_test(NAME lanlink-packet-crypto COMMAND lanlink_packet_crypto_tests)

    add_executable(lanlink_packet_bridge_tests
        tests/packet_bridge_tests.cpp
    )

    target_link_libraries(lanlink_packet_bridge_tests PRIVATE lanlink_transport lanlink_auth)
    lanlink_enable_warnings(lanlink_packet_bridge_tests)
    add_test(NAME lanlink-packet-bridge COMMAND lanlink_packet_bridge_tests)

    add_executable(lanlink_fanout_tests
        tests/fanout_tests.cpp
    )

    target_link_libraries(lanlink_fanout_tests PRIVATE lanlink_relay_domain lanlink_auth)
    lanlink_enable_warnings(lanlink_fanout_tests)
    add_test(NAME lanlink-fanout COMMAND lanlink_fanout_tests)

    add_executable(lanlink_transport_tests
        tests/transport_tests.cpp
    )

    target_link_libraries(lanlink_transport_tests PRIVATE lanlink_transport)
    lanlink_enable_warnings(lanlink_transport_tests)
    add_test(NAME lanlink-transport COMMAND lanlink_transport_tests)

    add_executable(lanlink_storage_tests
        tests/storage_tests.cpp
    )

    target_link_libraries(
        lanlink_storage_tests
        PRIVATE
            lanlink_storage
            ${LANLINK_SQLITE_TARGET}
    )
    lanlink_enable_warnings(lanlink_storage_tests)
    add_test(NAME lanlink-storage COMMAND lanlink_storage_tests)

    add_executable(lanlink_virtual_ipv4_tests
        tests/virtual_ipv4_tests.cpp
    )
    target_link_libraries(lanlink_virtual_ipv4_tests PRIVATE lanlink_relay_domain ${LANLINK_SQLITE_TARGET})
    lanlink_enable_warnings(lanlink_virtual_ipv4_tests)
    add_test(NAME lanlink-virtual-ipv4 COMMAND lanlink_virtual_ipv4_tests)

    add_executable(lanlink_network_service_tests
        tests/network_service_tests.cpp
    )

    target_link_libraries(lanlink_network_service_tests PRIVATE lanlink_relay_domain)
    lanlink_enable_warnings(lanlink_network_service_tests)
    add_test(NAME lanlink-network-service COMMAND lanlink_network_service_tests)

    add_executable(lanlink_network_control_channel_tests
        tests/network_control_channel_tests.cpp
    )

    target_link_libraries(
        lanlink_network_control_channel_tests
        PRIVATE
            lanlink_relay_domain
            lanlink_auth
            ${LANLINK_SQLITE_TARGET}
    )
    lanlink_enable_warnings(lanlink_network_control_channel_tests)
    add_test(NAME lanlink-network-control-channel COMMAND lanlink_network_control_channel_tests)

    add_executable(lanlink_packet_routing_tests
        tests/packet_routing_tests.cpp
    )
    target_link_libraries(lanlink_packet_routing_tests PRIVATE lanlink_relay_domain)
    lanlink_enable_warnings(lanlink_packet_routing_tests)
    add_test(NAME lanlink-packet-routing COMMAND lanlink_packet_routing_tests)

    if(UNIX)
        find_program(LANLINK_OPENSSL_EXECUTABLE openssl)
        find_program(LANLINK_BASH_EXECUTABLE bash)

        if(LANLINK_OPENSSL_EXECUTABLE AND LANLINK_BASH_EXECUTABLE)
            add_executable(lanlink_network_client_tests
                tests/network_client_tests.cpp
            )
            target_link_libraries(
                lanlink_network_client_tests
                PRIVATE
                    lanlink_transport
                    lanlink_relay_domain
                    lanlink_auth
                    ${LANLINK_SQLITE_TARGET}
            )
            lanlink_enable_warnings(lanlink_network_client_tests)
            add_test(
                NAME lanlink-network-client
                COMMAND
                    ${LANLINK_BASH_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/network_client_integration.sh
                    ${LANLINK_OPENSSL_EXECUTABLE}
                    $<TARGET_FILE:lanlink_network_client_tests>
            )
            set_tests_properties(lanlink-network-client PROPERTIES TIMEOUT 40)
        endif()
    endif()
endif()
