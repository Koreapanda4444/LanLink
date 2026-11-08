cmake_minimum_required(VERSION 3.24)
list(PREPEND VCPKG_OVERLAY_PORTS "${CMAKE_CURRENT_LIST_DIR}/cmake/ports")

option(LANLINK_BUILD_DESKTOP "Build the portable SDL/OpenGL desktop" OFF)
if(LANLINK_BUILD_DESKTOP)
    list(APPEND VCPKG_MANIFEST_FEATURES portable-desktop)
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum supported macOS version")
    list(PREPEND VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_LIST_DIR}/cmake/triplets")
endif()

project(LanLink VERSION 1.0.0 LANGUAGES CXX)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(msquic CONFIG REQUIRED)
find_package(OpenSSL 3.6.4 REQUIRED COMPONENTS SSL Crypto)
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
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8>")
    else()
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>")
    endif()
endfunction()

add_library(lanlink_core STATIC
    src/core/localization.cpp
    src/core/error.cpp
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
    src/protocol/policy_preview.cpp
    src/protocol/audit_event.cpp
    src/protocol/network_policy.cpp
    src/protocol/network_management.cpp
    src/protocol/network_overview.cpp
    src/protocol/network_workflow.cpp
    src/protocol/join_target.cpp
    src/protocol/user_access.cpp
    src/protocol/ipv4.cpp
    src/protocol/game_profiles.cpp
    src/protocol/game_diagnostics.cpp
    src/network/ethernet.cpp
    src/p2p/candidates.cpp
    src/protocol/path_diagnostics.cpp
    src/protocol/capabilities.cpp
    src/protocol/codec.cpp
    src/protocol/device_registration.cpp
    src/protocol/server_profiles.cpp
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
    src/auth/relay_health.cpp
    src/auth/directory.cpp
    src/auth/access_secret.cpp
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
    src/client/profile_store.cpp
    src/storage/relay_store.cpp
)

target_include_directories(lanlink_storage
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(lanlink_storage PRIVATE ${LANLINK_SQLITE_TARGET} PUBLIC lanlink_protocol lanlink_auth)

lanlink_enable_warnings(lanlink_storage)

add_library(lanlink_relay_domain STATIC
    src/relay/device_authority.cpp
    src/relay/forwarding_quota.cpp
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

add_library(lanlink_packet_engine STATIC src/network/routed_network.cpp src/transport/packet_bridge.cpp src/network/virtual_network.cpp src/network/multicast.cpp src/network/lan_discovery.cpp src/network/packet_history.cpp)
target_include_directories(lanlink_packet_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lanlink_packet_engine PUBLIC lanlink_auth lanlink_protocol lanlink_core PRIVATE OpenSSL::Crypto)
lanlink_enable_warnings(lanlink_packet_engine)

if(APPLE)
    add_library(lanlink_native_tun STATIC src/network/posix_tun.cpp src/platform/macos_tun.cpp)
    target_include_directories(lanlink_native_tun PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(lanlink_native_tun PUBLIC lanlink_packet_engine)
    lanlink_enable_warnings(lanlink_native_tun)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(lanlink_native_tun STATIC src/network/posix_tun.cpp src/platform/linux_tun.cpp)
    target_include_directories(lanlink_native_tun PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(lanlink_native_tun PUBLIC lanlink_packet_engine)
    lanlink_enable_warnings(lanlink_native_tun)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(lanlink_linux_tap STATIC src/network/linux_tap.cpp)
    target_include_directories(lanlink_linux_tap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(lanlink_linux_tap PUBLIC lanlink_packet_engine)
    lanlink_enable_warnings(lanlink_linux_tap)
endif()

add_executable(lanlink_directory_sign apps/relay/directory_sign.cpp)

target_link_libraries(lanlink_directory_sign PRIVATE lanlink_auth lanlink_core)
set_target_properties(lanlink_directory_sign PROPERTIES OUTPUT_NAME "lanlink-directory-sign")
lanlink_enable_warnings(lanlink_directory_sign)

add_library(lanlink_p2p STATIC src/p2p/local_candidates.cpp src/p2p/udp_socket.cpp src/p2p/stun.cpp src/p2p/punch.cpp src/p2p/connectivity.cpp src/p2p/peer_authorization.cpp src/p2p/path_quality.cpp)
target_include_directories(lanlink_p2p PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lanlink_p2p PUBLIC lanlink_auth lanlink_core PRIVATE OpenSSL::Crypto)
if(WIN32)
    target_compile_definitions(lanlink_p2p PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(lanlink_p2p PRIVATE iphlpapi ws2_32)
endif()
lanlink_enable_warnings(lanlink_p2p)


add_library(lanlink_transport STATIC
    src/transport/relay_bridge.cpp
    src/transport/relay_pool.cpp
    src/transport/relay_probe.cpp
    src/transport/directory_fetch.cpp
    src/p2p/direct_quic.cpp
    src/transport/device_registration.cpp
    src/transport/quic.cpp
    src/transport/reconnect.cpp
    src/transport/resource_limits.cpp
    src/transport/tls.cpp
)

target_include_directories(lanlink_transport
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(lanlink_transport
    PUBLIC
        lanlink_core
        lanlink_p2p
        lanlink_packet_engine
    PRIVATE
        lanlink_auth
        lanlink_protocol
        lanlink_relay_domain
        ${LANLINK_MSQUIC_TARGET}
        OpenSSL::SSL
)

if(WIN32)
    target_link_libraries(lanlink_transport PRIVATE ntdll ws2_32 winhttp)

    target_sources(lanlink_transport PRIVATE src/transport/windows_gateway.cpp src/transport/wintun_adapter.cpp src/transport/windows_certificate.cpp)
    target_link_libraries(lanlink_transport PRIVATE iphlpapi Crypt32 ncrypt fwpuclnt rpcrt4 uuid)
endif()

lanlink_enable_warnings(lanlink_transport)

add_library(lanlink_session STATIC src/session/relay_session.cpp src/session/relay_packet_path.cpp src/session/peer_packet_path.cpp)
target_include_directories(lanlink_session PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lanlink_session PUBLIC lanlink_transport)
lanlink_enable_warnings(lanlink_session)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(lanlink_l2 apps/service/linux_l2.cpp)
    target_link_libraries(lanlink_l2 PRIVATE lanlink_session lanlink_linux_tap)
    set_target_properties(lanlink_l2 PROPERTIES OUTPUT_NAME "lanlink-l2")
    lanlink_enable_warnings(lanlink_l2)
endif()

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

    add_library(lanlink_client_runtime STATIC apps/service/client_runtime.cpp apps/service/profile_controller.cpp src/platform/windows_adapter.cpp)
    target_link_libraries(lanlink_client_runtime PUBLIC lanlink_storage lanlink_session lanlink_local_ipc PRIVATE OpenSSL::Crypto)
    lanlink_enable_warnings(lanlink_client_runtime)

    add_executable(lanlink_service
        apps/service/main.cpp
        apps/service/windows_service.cpp
    )

    target_link_libraries(lanlink_service PRIVATE lanlink_client_runtime advapi32 shell32 ole32 uuid)
    set_target_properties(lanlink_service PROPERTIES OUTPUT_NAME "lanlink-service")
    lanlink_enable_warnings(lanlink_service)

    add_executable(lanlink_ui WIN32
        apps/ui/management_panel.cpp
        apps/ui/preferences.cpp
        apps/ui/main.cpp
        apps/ui/network_model.cpp
    )

    target_link_libraries(lanlink_ui PRIVATE lanlink_core lanlink_local_ipc advapi32 imgui::imgui d3d11 dxgi shell32 windowscodecs ole32)
    set_target_properties(lanlink_ui PROPERTIES OUTPUT_NAME "lanlink-ui")
    lanlink_enable_warnings(lanlink_ui)

    add_executable(lanlink_ui_cli apps/ui/control_cli.cpp)
    target_link_libraries(lanlink_ui_cli PRIVATE lanlink_core lanlink_local_ipc)
    set_target_properties(lanlink_ui_cli PROPERTIES OUTPUT_NAME "lanlink-ui-cli")
    lanlink_enable_warnings(lanlink_ui_cli)
endif()

if(UNIX)
    add_library(lanlink_local_ipc STATIC src/transport/posix_control.cpp)
    target_include_directories(lanlink_local_ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(lanlink_local_ipc PUBLIC lanlink_protocol Threads::Threads)
    lanlink_enable_warnings(lanlink_local_ipc)
    add_library(lanlink_client_runtime STATIC apps/service/client_runtime.cpp apps/service/profile_controller.cpp src/platform/posix_adapter.cpp)
    target_link_libraries(lanlink_client_runtime PUBLIC lanlink_storage lanlink_session lanlink_local_ipc lanlink_native_tun PRIVATE OpenSSL::Crypto)
    lanlink_enable_warnings(lanlink_client_runtime)
    add_executable(lanlink_service apps/service/posix_main.cpp)
    target_link_libraries(lanlink_service PRIVATE lanlink_client_runtime)
    set_target_properties(lanlink_service PROPERTIES OUTPUT_NAME "lanlink-service")
    lanlink_enable_warnings(lanlink_service)
    add_executable(lanlink_ui_cli apps/ui/control_cli.cpp)
    target_link_libraries(lanlink_ui_cli PRIVATE lanlink_core lanlink_local_ipc)
    set_target_properties(lanlink_ui_cli PROPERTIES OUTPUT_NAME "lanlink-ui-cli")
    lanlink_enable_warnings(lanlink_ui_cli)
endif()

if(UNIX AND LANLINK_BUILD_DESKTOP)
    find_package(SDL3 CONFIG REQUIRED)
    find_package(imgui CONFIG REQUIRED)
    find_package(OpenGL REQUIRED)
    find_package(PNG REQUIRED)
    add_executable(lanlink_ui apps/ui/posix_main.cpp apps/ui/posix_preferences.cpp apps/ui/network_model.cpp apps/ui/management_panel.cpp)
    target_compile_definitions(lanlink_ui PRIVATE LANLINK_LOGO_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/branding/lanlink.png")
    target_link_libraries(lanlink_ui PRIVATE lanlink_core lanlink_local_ipc imgui::imgui SDL3::SDL3 OpenGL::GL PNG::PNG)
    set_target_properties(lanlink_ui PROPERTIES OUTPUT_NAME "lanlink-ui")
    lanlink_enable_warnings(lanlink_ui)
    if(APPLE)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(icon "${CMAKE_CURRENT_BINARY_DIR}/LanLink.icns")
        add_custom_command(OUTPUT "${icon}"
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/make_icon.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/branding/lanlink.png" "${icon}"
            DEPENDS assets/branding/lanlink.png packaging/macos/make_icon.py)
        target_sources(lanlink_ui PRIVATE "${icon}" assets/branding/lanlink.png)
        set_source_files_properties("${icon}" assets/branding/lanlink.png PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(lanlink_ui PROPERTIES MACOSX_BUNDLE TRUE OUTPUT_NAME LanLink
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}" MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    endif()
endif()

include(cmake/WindowsResources.cmake)
include(cmake/BuildMetadata.cmake)
include(cmake/ClientPackage.cmake)
include(cmake/RelayPackage.cmake)
include(cmake/PosixClientPackage.cmake)
