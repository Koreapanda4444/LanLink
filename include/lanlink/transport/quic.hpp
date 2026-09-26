#pragma once

#include "lanlink/auth/network_keys.hpp"
#include "lanlink/protocol/network_messages.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace lanlink::core {
class Logger;
}

namespace lanlink::relay {
class NetworkControlChannel;
}

namespace lanlink::transport {

struct QuicServerOptions {
    std::string listen_host = "0.0.0.0";
    std::uint16_t port = 4433;
    std::filesystem::path certificate_file;
    std::filesystem::path private_key_file;
    std::filesystem::path auth_token_file;
    std::chrono::milliseconds handshake_timeout{10'000};
    std::chrono::milliseconds authentication_timeout{10'000};
    std::chrono::milliseconds idle_timeout{60'000};
    std::uint32_t keep_alive_interval_ms = 15'000;
};

struct QuicClientOptions {
    std::string relay_host = "127.0.0.1";
    std::uint16_t port = 4433;
    std::filesystem::path identity_file;
    std::filesystem::path auth_token_file;
    std::chrono::milliseconds handshake_timeout{10'000};
    std::chrono::milliseconds authentication_timeout{10'000};
    std::chrono::milliseconds idle_timeout{60'000};
    std::uint32_t keep_alive_interval_ms = 15'000;
    std::chrono::milliseconds reconnect_initial_delay{1'000};
    std::chrono::milliseconds reconnect_maximum_delay{30'000};
};

struct NetworkListReply {
    protocol::NetworkResultCode code = protocol::NetworkResultCode::success;
    protocol::NetworkListResult result;

    bool operator==(const NetworkListReply&) const = default;
};

struct NetworkPeerStateReply {
    protocol::NetworkResultCode code = protocol::NetworkResultCode::success;
    std::optional<protocol::NetworkPeerState> state;

    bool operator==(const NetworkPeerStateReply&) const = default;
};

struct NetworkKeySnapshot {
    std::uint64_t epoch;
    auth::NetworkKey key;

    bool operator==(const NetworkKeySnapshot&) const = default;
};

class QuicRelayServer {
public:
    QuicRelayServer(QuicServerOptions options,
                    core::Logger& logger,
                    relay::NetworkControlChannel& control_channel);
    ~QuicRelayServer();

    QuicRelayServer(const QuicRelayServer&) = delete;
    QuicRelayServer& operator=(const QuicRelayServer&) = delete;
    QuicRelayServer(QuicRelayServer&&) = delete;
    QuicRelayServer& operator=(QuicRelayServer&&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class QuicRelayClient {
public:
    QuicRelayClient(QuicClientOptions options, core::Logger& logger);
    ~QuicRelayClient();

    QuicRelayClient(const QuicRelayClient&) = delete;
    QuicRelayClient& operator=(const QuicRelayClient&) = delete;
    QuicRelayClient(QuicRelayClient&&) = delete;
    QuicRelayClient& operator=(QuicRelayClient&&) = delete;

    void run(std::stop_token stop_token);
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool authenticated() const noexcept;
    [[nodiscard]] std::optional<auth::SessionId> session_id() const noexcept;

    [[nodiscard]] protocol::NetworkOperationResult create_network(
        std::string name,
        std::chrono::milliseconds timeout = std::chrono::seconds{10});
    [[nodiscard]] NetworkListReply list_networks(
        std::chrono::milliseconds timeout = std::chrono::seconds{10});
    [[nodiscard]] protocol::NetworkOperationResult join_network(
        const protocol::NetworkId& network_id,
        std::chrono::milliseconds timeout = std::chrono::seconds{10});
    [[nodiscard]] protocol::NetworkOperationResult leave_network(
        const protocol::NetworkId& network_id,
        std::chrono::milliseconds timeout = std::chrono::seconds{10});
    [[nodiscard]] protocol::NetworkOperationResult invite_member(
        const protocol::NetworkId& network_id,
        const protocol::NetworkDeviceId& device_id,
        std::chrono::milliseconds timeout = std::chrono::seconds{10});
    [[nodiscard]] protocol::NetworkOperationResult approve_member(
        const protocol::NetworkId& network_id,
        const protocol::NetworkDeviceId& device_id,
        std::chrono::milliseconds timeout = std::chrono::seconds{10});
    [[nodiscard]] protocol::NetworkOperationResult kick_member(
        const protocol::NetworkId& network_id,
        const protocol::NetworkDeviceId& device_id,
        std::chrono::milliseconds timeout = std::chrono::seconds{10});
    [[nodiscard]] NetworkPeerStateReply fetch_peer_state(
        const protocol::NetworkId& network_id,
        std::chrono::milliseconds timeout = std::chrono::seconds{10});
    [[nodiscard]] std::optional<protocol::NetworkPeerState> cached_peer_state(
        const protocol::NetworkId& network_id) const;
    [[nodiscard]] std::vector<protocol::NetworkPeerState> cached_peer_states() const;
    [[nodiscard]] std::optional<NetworkKeySnapshot> cached_network_key(
        const protocol::NetworkId& network_id) const;
    void set_network_event_handler(std::function<void(const protocol::NetworkEvent&)> handler);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
