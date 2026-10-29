#include "lanlink/transport/packet_bridge.hpp"
#include "lanlink/protocol/ipv4_fanout.hpp"
#include "lanlink/protocol/ipv4.hpp"

#include <algorithm>
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

    for (std::size_t index = 0; index < networks.size(); ++index) {
        const auto& state = networks[index].peers;
        if (!protocol::is_ipv4_host(state.own_address, state.subnet_address, state.prefix_length))
            throw std::invalid_argument("virtual network address is not a subnet host");
        for (std::size_t other = 0; other < index; ++other) {
            const auto& previous = networks[other].peers;
            const auto mask = 0xffffffffU << (32U - std::min(state.prefix_length, previous.prefix_length));
            if (state.network_id == previous.network_id ||
                (state.subnet_address & mask) == (previous.subnet_address & mask))
                throw std::invalid_argument("virtual network subnets overlap");
        }
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
                std::erase_if(old->second->inbound_discovery, [&](const auto& entry) {
                    return std::none_of(state.peers.begin(), state.peers.end(),
                        [&](const auto& peer) { return peer.device_id == entry.first; });
                });
            }
            updated.emplace(state.network_id, old->second);
            continue;
        }

        auto session = std::make_shared<Session>(Session{
            state, network.key, identity->signed_key,
            std::make_shared<auth::NetworkPacketCipher>(
                state.network_id, state.key_epoch, network.key.key,
                identity->device_id, identity->signed_key, state.own_address), {}, {}, {}, {}, 0});
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
    const auto header = protocol::inspect_ipv4(ipv4_packet);
    std::shared_ptr<Session> session;
    std::uint16_t mtu = 0;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, candidate] : sessions_) {
            static_cast<void>(id);
            const auto& state = candidate->peers;
            if (protocol::is_ipv4_fanout_destination(destination, state.subnet_address, state.prefix_length) &&
                candidate->injected_fanout.contains(ipv4_packet, network::PacketHistory::Clock::now())) {
                ++candidate->reflected; return;
            }
            const auto mask = 0xffffffffU << (32U - state.prefix_length);
            if (source == state.own_address &&
                (ipv4_packet[9] == std::byte{2} || (destination & mask) == state.subnet_address ||
                 protocol::is_ipv4_fanout_destination(
                     destination, state.subnet_address, state.prefix_length))) {
                session = candidate;
                mtu = mtu_;
                if (candidate->multicast.observe(ipv4_packet, network::MulticastMembership::Clock::now()) !=
                    network::MulticastMembership::Report::not_igmp) return;
                if (protocol::is_ipv4_fanout_destination(destination, state.subnet_address, state.prefix_length) &&
                    ipv4_packet.size() > std::min<std::uint16_t>(mtu, 1400)) return;
                if (protocol::is_ipv4_fanout_destination(destination, state.subnet_address, state.prefix_length) &&
                    !candidate->outbound_discovery.permit(ipv4_packet, network::LanDiscoveryGuard::Clock::now())) return;
                break;
            }
        }
    }
    if (session) {
        if (ipv4_packet.size() > mtu && header.dont_fragment) {
            if (const auto error = protocol::ipv4_fragmentation_needed(ipv4_packet, source, mtu)) inject_(*error);
            return;
        }
        for (const auto& fragment : protocol::fragment_ipv4(ipv4_packet, mtu)) send_(session->cipher->encrypt(fragment));
    }
}

void VirtualPacketBridge::inbound(const protocol::ForwardedNetworkPacket& delivery) {
    std::shared_ptr<Session> session;
    protocol::NetworkPeerState peers;
    std::uint16_t mtu = 0;
    {
        std::lock_guard lock(mutex_);
        const auto found = sessions_.find(delivery.packet.network_id);
        if (found == sessions_.end()) {
            return;
        }
        session = found->second;
        peers = session->peers;
        mtu = mtu_;
    }
    const auto plaintext = session->cipher->decrypt(delivery.packet,
                                                     delivery.sender_device_id);
    const auto header = protocol::inspect_ipv4(plaintext);
    const auto current = [&] {
        const auto found = sessions_.find(delivery.packet.network_id);
        if (found == sessions_.end() || found->second != session) return false;
        const auto before = std::find_if(peers.peers.begin(), peers.peers.end(),
            [&](const auto& peer) { return peer.device_id == delivery.sender_device_id; });
        return before != peers.peers.end() && std::find(session->peers.peers.begin(), session->peers.peers.end(), *before) != session->peers.peers.end();
    };
    if (protocol::is_ipv4_fanout_destination(delivery.packet.destination_ipv4,
                                            peers.subnet_address, peers.prefix_length)) {
        std::lock_guard lock(mutex_);
        if (!current()) return;
        if (plaintext.size() > std::min<std::uint16_t>(mtu, 1400)) return;
        if (protocol::is_ipv4_multicast(delivery.packet.destination_ipv4) &&
            !session->multicast.receives(delivery.packet.destination_ipv4, read_ipv4(plaintext, 12),
                                        network::MulticastMembership::Clock::now())) return;
        if (!session->inbound_discovery[delivery.sender_device_id].permit(plaintext,
                network::LanDiscoveryGuard::Clock::now())) return;
        session->injected_fanout.remember(plaintext, network::PacketHistory::Clock::now());
    }
    if (plaintext.size() > mtu && header.dont_fragment) {
        if (const auto error = protocol::ipv4_fragmentation_needed(plaintext, peers.own_address, mtu))
            send_(session->cipher->encrypt(*error));
        return;
    }
    for (const auto& fragment : protocol::fragment_ipv4(plaintext, mtu)) {
        std::lock_guard lock(mutex_);
        if (!current()) return;
        inject_(fragment);
    }
}

std::uint64_t VirtualPacketBridge::reflected_packets() const {
    std::lock_guard lock(mutex_);
    std::uint64_t count = 0;
    for (const auto& [id, session] : sessions_) { static_cast<void>(id); count += session->reflected; }
    return count;
}

protocol::GameDiagnostics VirtualPacketBridge::game_diagnostics() const {
    std::lock_guard lock(mutex_);
    protocol::GameDiagnostics result;
    result.mtu = mtu_;
    for (const auto& [id, session] : sessions_) {
        if (result.networks.size() == protocol::max_game_diagnostic_networks) {
            result.truncated = true; break;
        }
        protocol::GameNetworkStatus item;
        item.network_id = id; item.address = session->peers.own_address;
        item.subnet = session->peers.subnet_address; item.prefix_length = session->peers.prefix_length;
        item.online_peers = static_cast<std::uint32_t>(std::count_if(
            session->peers.peers.begin(), session->peers.peers.end(),
            [](const auto& peer) { return peer.signed_key.has_value(); }));
        const auto outgoing = session->outbound_discovery.counters();
        item.discovery_sent = outgoing.forwarded; item.rate_limited = outgoing.rate_limited;
        item.duplicates = outgoing.duplicate; item.invalid = outgoing.invalid; item.reflections = session->reflected;
        for (const auto& [peer, guard] : session->inbound_discovery) {
            static_cast<void>(peer);
            const auto incoming = guard.counters();
            item.discovery_received += incoming.forwarded; item.rate_limited += incoming.rate_limited;
            item.duplicates += incoming.duplicate; item.invalid += incoming.invalid;
        }
        result.networks.push_back(item);
    }
    return result;
}

void VirtualPacketBridge::set_mtu(const std::uint16_t mtu) {
    if (mtu < 68) throw std::invalid_argument("virtual MTU is below 68");
    std::lock_guard lock(mutex_); mtu_ = mtu;
}

void VirtualPacketBridge::synchronize_multicast(const std::optional<std::vector<std::uint32_t>>& groups) {
    std::lock_guard lock(mutex_);
    for (auto& [id, session] : sessions_) { static_cast<void>(id); session->multicast.synchronize(groups); }
}

void VirtualPacketBridge::clear() noexcept {
    std::lock_guard lock(mutex_);
    sessions_.clear();
}

}
