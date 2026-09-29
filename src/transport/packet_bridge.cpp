#include "lanlink/transport/packet_bridge.hpp"
#include "lanlink/protocol/ipv4_fanout.hpp"

#include <stdexcept>
#include <utility>

namespace lanlink::transport {
namespace {

std::uint32_t read_ipv4(const std::span<const std::byte> packet,
                        const std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value = (value << 8U) | std::to_integer<unsigned char>(packet[offset + index]);
    }
    return value;
}

}

VirtualPacketBridge::VirtualPacketBridge(SendHandler send, InjectHandler inject)
    : send_(std::move(send)), inject_(std::move(inject)) {
    if (!send_ || !inject_) {
        throw std::invalid_argument("virtual packet bridge needs send and inject handlers");
    }
}

void VirtualPacketBridge::synchronize(
    const std::optional<PacketIdentitySnapshot>& identity,
    const std::span<const ActiveNetworkPacketState> networks) {
    std::lock_guard lock(mutex_);
    if (!identity) {
        sessions_.clear();
        return;
    }

    std::map<protocol::NetworkId, std::shared_ptr<Session>> updated;
    for (const auto& network : networks) {
        const auto& state = network.peers;
        if (state.key_epoch == 0 || state.key_epoch != network.key.epoch ||
            state.own_address == 0 || state.prefix_length == 0 ||
            state.prefix_length > 30 ||
            (state.own_address & (0xffffffffU << (32U - state.prefix_length))) !=
                state.subnet_address) {
            throw std::invalid_argument("virtual packet bridge has invalid network state");
        }
        const auto old = sessions_.find(state.network_id);
        if (old != sessions_.end() && old->second->key == network.key &&
            old->second->peers.own_address == state.own_address &&
            old->second->own_signed_key == identity->signed_key) {
            if (old->second->peers != state) {
                old->second->cipher->update_peers(state);
                old->second->peers = state;
            }
            updated.emplace(state.network_id, old->second);
            continue;
        }

        auto session = std::make_shared<Session>(Session{
            state, network.key, identity->signed_key,
            std::make_shared<auth::NetworkPacketCipher>(
                state.network_id, state.key_epoch, network.key.key,
                identity->device_id, identity->signed_key, state.own_address)});
        session->cipher->update_peers(state);
        if (!updated.emplace(state.network_id, std::move(session)).second) {
            throw std::invalid_argument("duplicate virtual packet network");
        }
    }
    sessions_.swap(updated);
}

void VirtualPacketBridge::outbound(const std::span<const std::byte> ipv4_packet) {
    if (ipv4_packet.size() < protocol::min_virtual_ipv4_packet_size) {
        return;
    }
    const auto source = read_ipv4(ipv4_packet, 12);
    const auto destination = read_ipv4(ipv4_packet, 16);
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, candidate] : sessions_) {
            static_cast<void>(id);
            const auto& state = candidate->peers;
            const auto mask = 0xffffffffU << (32U - state.prefix_length);
            if (source == state.own_address &&
                ((destination & mask) == state.subnet_address ||
                 protocol::is_ipv4_fanout_destination(
                     destination, state.subnet_address, state.prefix_length))) {
                session = candidate;
                break;
            }
        }
    }
    if (session) {
        send_(session->cipher->encrypt(ipv4_packet));
    }
}

void VirtualPacketBridge::inbound(const protocol::ForwardedNetworkPacket& delivery) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        const auto found = sessions_.find(delivery.packet.network_id);
        if (found == sessions_.end()) {
            return;
        }
        session = found->second;
    }
    const auto plaintext = session->cipher->decrypt(delivery.packet,
                                                     delivery.sender_device_id);
    inject_(plaintext);
}

void VirtualPacketBridge::clear() noexcept {
    std::lock_guard lock(mutex_);
    sessions_.clear();
}

}
