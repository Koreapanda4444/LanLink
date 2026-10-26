#include "lanlink/core/error.hpp"
#include <algorithm>
#include <cctype>

namespace lanlink::core {
ErrorCode classify_error(std::string_view message) noexcept {
    const auto contains = [&](std::string_view word) {
        return std::search(message.begin(), message.end(), word.begin(), word.end(), [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        }) != message.end();
    };
    if (contains("auth") || contains("certificate") || contains("credential")) return ErrorCode::authentication_failed;
    if (contains("timeout") || contains("timed out")) return ErrorCode::timeout;
    if (contains("queue") || contains("backpressure")) return ErrorCode::backpressure;
    if (contains("revok") || contains("epoch")) return ErrorCode::revoked;
    if (contains("packet")) return ErrorCode::packet_rejected;
    if (contains("adapter") || contains("wintun")) return ErrorCode::adapter_failed;
    if (contains("unsupported") || contains("capability")) return ErrorCode::unsupported;
    return ErrorCode::connection_failed;
}
std::string_view error_summary(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::none: return "";
    case ErrorCode::connection_failed: return "Connection unavailable";
    case ErrorCode::authentication_failed: return "Peer authentication failed";
    case ErrorCode::timeout: return "Connection timed out";
    case ErrorCode::backpressure: return "Send queue is full";
    case ErrorCode::packet_rejected: return "Network packet rejected";
    case ErrorCode::adapter_failed: return "Virtual adapter failed";
    case ErrorCode::unsupported: return "Peer does not support this feature";
    case ErrorCode::revoked: return "Network membership or session key changed";
    }
    return "Connection unavailable";
}
}
