#pragma once
#include <cstdint>
#include <string_view>

namespace lanlink::core {
enum class ErrorCode : std::uint8_t {
    none, connection_failed, authentication_failed, timeout, backpressure,
    packet_rejected, adapter_failed, unsupported, revoked,
};
[[nodiscard]] ErrorCode classify_error(std::string_view message) noexcept;
[[nodiscard]] std::string_view error_summary(ErrorCode code) noexcept;
}
