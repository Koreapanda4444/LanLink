#pragma once

#include "lanlink/protocol/message.hpp"
#include "lanlink/protocol/capabilities.hpp"
#include "lanlink/relay/network_service.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace lanlink::relay {

struct RoutedControlFrame {
    auth::DeviceId recipient_device_id{};
    protocol::Frame frame;

    bool operator==(const RoutedControlFrame&) const = default;
};

struct ControlDispatch {
    protocol::Frame response;
    std::vector<RoutedControlFrame> events;

    bool operator==(const ControlDispatch&) const = default;
};

class NetworkControlChannel {
public:
    using TimeSource = std::function<std::int64_t()>;

    explicit NetworkControlChannel(NetworkService& service, TimeSource time_source = {});

    NetworkControlChannel(const NetworkControlChannel&) = delete;
    NetworkControlChannel& operator=(const NetworkControlChannel&) = delete;
    NetworkControlChannel(NetworkControlChannel&&) = delete;
    NetworkControlChannel& operator=(NetworkControlChannel&&) = delete;

    void note_authenticated(const auth::DeviceId& actor);
    [[nodiscard]] std::vector<RoutedControlFrame> note_authenticated(
        const auth::DeviceId& actor,
        const auth::SessionId& session_id,
        const auth::SignedDeviceKey& signed_key);
    [[nodiscard]] std::vector<RoutedControlFrame> note_disconnected(
        const auth::DeviceId& actor, const auth::SessionId& session_id);
    [[nodiscard]] std::vector<RoutedControlFrame> initial_peer_states(
        const auth::DeviceId& actor);

    [[nodiscard]] ControlDispatch handle_authenticated(
        const auth::DeviceId& actor,
        const protocol::Frame& request);
    [[nodiscard]] std::vector<RoutedEncryptedPacket> route_encrypted_packet(
        const auth::DeviceId& actor, std::span<const std::byte> payload);
    [[nodiscard]] protocol::NegotiatedCapabilities capabilities(const auth::DeviceId& actor) const;

private:
    [[nodiscard]] std::int64_t now_ms() const;

    NetworkService& service_;
    TimeSource time_source_;
    struct CapabilitySession {
        auth::SessionId session{};
        protocol::NegotiatedCapabilities negotiated;
        std::map<std::pair<protocol::NetworkId, auth::DeviceId>, std::uint64_t> generations;
    };
    mutable std::mutex capabilities_mutex_;
    std::map<auth::DeviceId, CapabilitySession> capabilities_;
};

}
