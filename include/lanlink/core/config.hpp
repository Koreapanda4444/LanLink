#pragma once

#include "lanlink/core/logger.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace lanlink::core {

struct RuntimeConfig {
    std::string bootstrap_mode = "manual";
    std::string directory_url;
    std::string directory_public_key;
    std::string directory_relay_id;
    std::uint16_t virtual_mtu = 1400;
    bool accept_routed_subnets=false;
    bool gateway_forwarding=false;
    bool p2p_enabled=true;
    std::string p2p_stun_host;
    std::uint16_t p2p_stun_port=3478;
    // No client server is selected on a fresh installation.
    std::string relay_host;
    std::uint16_t relay_port = 4433;
    std::string listen_host = "0.0.0.0";
    std::filesystem::path tls_certificate_file;
    std::filesystem::path tls_private_key_file;
    std::filesystem::path device_identity_file = "data/device.identity";
    std::filesystem::path auth_token_file = "data/auth.token";
    std::filesystem::path relay_database_file = "data/relay.db";
    std::string relay_id;
    std::string relay_authority;
    std::filesystem::path relay_health_identity_file;
    std::string relay_upstream_host,relay_upstream_id,relay_upstream_health_public_key;
    std::uint16_t relay_upstream_port=4433;
    std::uint32_t relay_upstream_capacity=128;
    std::uint32_t quic_handshake_timeout_ms = 10'000;
    std::uint32_t authentication_timeout_ms = 10'000;
    std::uint32_t quic_idle_timeout_ms = 60'000;
    std::uint32_t quic_keep_alive_interval_ms = 15'000;
    std::uint32_t reconnect_initial_delay_ms = 1'000;
    std::uint32_t reconnect_max_delay_ms = 30'000;
    std::uint32_t relay_max_connections = 128;
    std::uint32_t relay_max_tls_handshakes = 16;
    std::uint32_t relay_send_queue_bytes = 4U * 1024U * 1024U;
    std::uint32_t relay_send_queue_frames = 256;
    std::uint32_t relay_control_requests_per_second = 32;
    std::uint32_t relay_health_requests_per_second=64;
    std::uint32_t relay_device_bytes_per_second=8U*1024U*1024U,relay_network_bytes_per_second=32U*1024U*1024U;
    std::uint32_t relay_authority_bytes_per_second=64U*1024U*1024U;
    std::uint32_t relay_device_deliveries_per_second=8192,relay_network_deliveries_per_second=32768,relay_authority_deliveries_per_second=65536;
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
