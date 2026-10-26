#pragma once

#include "lanlink/auth/network_keys.hpp"
#include "lanlink/protocol/network_messages.hpp"

#include <optional>
#include <vector>

namespace lanlink::session {

struct NetworkKeySnapshot {
    std::uint64_t epoch;
    auth::NetworkKey key;
    bool operator==(const NetworkKeySnapshot&) const = default;
};

struct PacketIdentitySnapshot {
    auth::DeviceId device_id{};
    auth::SignedDeviceKey signed_key;
    bool operator==(const PacketIdentitySnapshot&) const = default;
};

struct ActiveNetworkPacketState {
    protocol::NetworkPeerState peers;
    NetworkKeySnapshot key;
};

struct NetworkSnapshot {
    std::optional<PacketIdentitySnapshot> identity;
    std::vector<ActiveNetworkPacketState> networks;
};

}

namespace lanlink::transport {
using session::NetworkKeySnapshot;
using session::PacketIdentitySnapshot;
using session::ActiveNetworkPacketState;
}
