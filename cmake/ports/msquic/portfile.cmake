vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/microsoft/msquic.git
    REF 16ca4d8d785d34fb22285e7c652e477ac16475a5
)
set(tls_provider schannel)
set(tls_options)
if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_from_git(
        OUT_SOURCE_PATH XDP_SOURCE_PATH
        URL https://github.com/microsoft/xdp-for-windows.git
        REF d372b52577a724e04fa4c06acb90bbfa4719fc25
    )
    file(COPY "${XDP_SOURCE_PATH}/published/external" DESTINATION "${SOURCE_PATH}/submodules/xdp-for-windows/published")
else()
    set(tls_provider openssl)
    list(APPEND tls_options -DQUIC_USE_EXTERNAL_OPENSSL=ON)
endif()
# Keep upstream warnings, but do not make a new compiler warning a dependency build failure.
vcpkg_replace_string("${SOURCE_PATH}/CMakeLists.txt" "-Werror -Wall" "-Wall")
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS
    -DQUIC_SOURCE_LINK=OFF
    -DQUIC_BUILD_SHARED=ON
    -DQUIC_BUILD_TEST=OFF
    -DQUIC_BUILD_PERF=OFF
    -DQUIC_ENABLE_LOGGING=OFF
    -DQUIC_TLS_LIB=${tls_provider}
    ${tls_options}
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH share/msquic)
vcpkg_copy_pdbs()
if(VCPKG_TARGET_IS_OSX)
    set(platform CX_PLATFORM_DARWIN)
elseif(NOT VCPKG_TARGET_IS_WINDOWS)
    set(platform CX_PLATFORM_LINUX)
endif()
if(platform)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/quic_platform.h"
        "#elif ${platform}" "#elif 1\n#ifndef ${platform}\n#define ${platform}\n#endif")
endif()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE" "${SOURCE_PATH}/THIRD-PARTY-NOTICES")
