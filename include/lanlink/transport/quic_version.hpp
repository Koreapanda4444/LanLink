#pragma once
#include <msquic.h>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace lanlink::transport {
inline void require_secure_quic(const QUIC_API_TABLE* api) {
    std::array<std::uint32_t, 4> version{};
    auto size = static_cast<std::uint32_t>(sizeof(version));
    if (QUIC_FAILED(api->GetParam(nullptr, QUIC_PARAM_GLOBAL_LIBRARY_VERSION, &size, version.data())) ||
        size != sizeof(version) || std::array{version[0], version[1], version[2]} < std::array{2U, 6U, 3U})
        throw std::runtime_error("MsQuic 2.6.3 or newer is required; replace the complete LanLink package");
}
}
