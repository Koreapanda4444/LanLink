#pragma once

#include "lanlink/core/logger.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace lanlink::core {

struct RuntimeConfig {
    std::string relay_host = "127.0.0.1";
    std::uint16_t relay_port = 4433;
    std::string listen_host = "0.0.0.0";
    std::filesystem::path tls_certificate_file;
    std::filesystem::path tls_private_key_file;
    std::filesystem::path device_identity_file = "data/device.identity";
    std::filesystem::path auth_token_file = "data/auth.token";
    std::uint32_t quic_handshake_timeout_ms = 10'000;
    std::uint32_t authentication_timeout_ms = 10'000;
    std::uint32_t quic_idle_timeout_ms = 60'000;
    std::uint32_t quic_keep_alive_interval_ms = 15'000;
    std::uint32_t reconnect_initial_delay_ms = 1'000;
    std::uint32_t reconnect_max_delay_ms = 30'000;
    LogLevel log_level = LogLevel::info;
    std::filesystem::path log_directory = "logs";
    std::uintmax_t log_max_size_bytes = 5U * 1024U * 1024U;
    std::size_t log_max_files = 5;
};

[[nodiscard]] RuntimeConfig parse_config(std::string_view content);
[[nodiscard]] RuntimeConfig load_config(const std::filesystem::path& path);
void validate_config(const RuntimeConfig& config);
void validate_relay_config(const RuntimeConfig& config);
void validate_service_config(const RuntimeConfig& config);

}
