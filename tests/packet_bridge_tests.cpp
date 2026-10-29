#include "lanlink/auth/handshake.hpp"
#include "lanlink/transport/packet_bridge.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

namespace auth = lanlink::auth;
namespace protocol = lanlink::protocol;
namespace transport = lanlink::transport;

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void authenticate(auth::ClientHandshake& client, auth::ServerHandshake& server) {
    const auto result = server.handle_proof(
        client.handle_challenge(server.handle_hello(client.begin(1))));
    require(client.handle_result(result), "packet bridge authentication failed");
}

std::vector<std::byte> ipv4_packet(const std::uint32_t source,
                                   const std::uint32_t destination) {
    std::vector<std::byte> bytes(24);
    bytes[0] = std::byte{0x45};
    bytes[3] = std::byte{24};
    bytes[8] = std::byte{64};
    bytes[9] = std::byte{17};
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[12 + index] = std::byte{static_cast<unsigned char>(
            source >> ((3U - index) * 8U))};
        bytes[16 + index] = std::byte{static_cast<unsigned char>(
            destination >> ((3U - index) * 8U))};
    }
    return bytes;
}

void test_bridge(const std::filesystem::path& directory) {
    const auto owner = auth::DeviceIdentity::load_or_create(directory / "owner.identity");
    const auto member = auth::DeviceIdentity::load_or_create(directory / "member.identity");
    const auto token = auth::AuthToken::from_secret(
        "lanlink-packet-bridge-integration-test-token");
    auth::ClientHandshake owner_handshake(owner, token);
    auth::ClientHandshake member_handshake(member, token);
    auth::ServerHandshake owner_server(
        token, 101, auth::ServerHandshake::Clock::now() + std::chrono::minutes{1});
    auth::ServerHandshake member_server(
        token, 102, auth::ServerHandshake::Clock::now() + std::chrono::minutes{1});
    authenticate(owner_handshake, owner_server);
    authenticate(member_handshake, member_server);

    protocol::NetworkId network{};
    network.front() = std::byte{17};
    constexpr std::uint32_t subnet = 0x0a400000U;
    constexpr std::uint32_t owner_ip = subnet + 1;
    constexpr std::uint32_t member_ip = subnet + 2;
    const auto key = auth::NetworkKey::random();
    protocol::NetworkPeerState owner_state{
        network, 1, subnet, 24, owner_ip,
        {{member.device_id(), member_ip, member_server.signed_device_key()}},
        owner.device_id(), 7};
    protocol::NetworkPeerState member_state{
        network, 1, subnet, 24, member_ip,
        {{owner.device_id(), owner_ip, owner_server.signed_device_key()}},
        owner.device_id(), 7};
    const transport::PacketIdentitySnapshot owner_identity{
        owner.device_id(), owner_server.signed_device_key()};
    const transport::PacketIdentitySnapshot member_identity{
        member.device_id(), member_server.signed_device_key()};
    const std::vector owner_networks{
        transport::ActiveNetworkPacketState{owner_state, {7, key}}};
    const std::vector member_networks{
        transport::ActiveNetworkPacketState{member_state, {7, key}}};

    std::vector<protocol::EncryptedNetworkPacket> sent;
    std::vector<std::vector<std::byte>> delivered;
    transport::VirtualPacketBridge owner_bridge(
        [&](const auto& packet) { sent.push_back(packet); },
        [](const auto) {});
    transport::VirtualPacketBridge member_bridge(
        [](const auto&) {},
        [&](const auto bytes) { delivered.emplace_back(bytes.begin(), bytes.end()); });
    owner_bridge.synchronize(owner_identity, owner_networks);
    member_bridge.synchronize(member_identity, member_networks);

    auto overlapping = owner_networks;
    auto conflicting = owner_networks.front();
    conflicting.peers.network_id.front() ^= std::byte{1};
    overlapping.push_back(conflicting);
    bool overlap_rejected = false;
    try { owner_bridge.synchronize(owner_identity, overlapping); }
    catch (const std::invalid_argument&) { overlap_rejected = true; }
    require(overlap_rejected, "overlapping networks cannot ambiguously route a broadcast");

    const auto plaintext = ipv4_packet(owner_ip, member_ip);
    owner_bridge.outbound(plaintext);
    require(sent.size() == 1 && sent[0].ciphertext != plaintext,
            "Wintun packet must leave as encrypted network packet");
    member_bridge.inbound({owner.device_id(), sent[0]});
    require(delivered.size() == 1 && delivered[0] == plaintext,
            "relay packet must decrypt before Wintun injection");
    try {
        member_bridge.inbound({owner.device_id(), sent[0]});
        throw std::runtime_error("replayed bridge delivery was accepted");
    } catch (const std::runtime_error& error) {
        require(std::string_view{error.what()} != "replayed bridge delivery was accepted",
                "bridge lost replay protection");
    }

    owner_bridge.synchronize(owner_identity, owner_networks);
    member_bridge.synchronize(member_identity, member_networks);
    owner_bridge.outbound(plaintext);
    require(sent.size() == 2 && sent[1].sequence == 2,
            "unchanged snapshots preserve sender sequence");
    member_bridge.inbound({owner.device_id(), sent[1]});
    require(delivered.size() == 2, "unchanged snapshots preserve receiver state");

    owner_bridge.outbound(ipv4_packet(owner_ip, subnet + 3));
    require(sent.size() == 3, "same subnet packet is sent to the relay");
    owner_bridge.outbound(ipv4_packet(owner_ip, 0x08080808U));
    require(sent.size() == 3, "unrelated traffic stays off the virtual network");

    for (const auto destination : {0xffffffffU, 0xe00000fbU}) {
        const auto discovery = ipv4_packet(owner_ip, destination);
        owner_bridge.outbound(discovery);
        require(sent.back().destination_ipv4 == destination,
                "Wintun discovery packet retains its routing destination");
        member_bridge.inbound({owner.device_id(), sent.back()});
        require(delivered.back() == discovery,
                "relay discovery packet returns to Wintun unchanged");
    }

    member_bridge.synchronize(std::nullopt, {});
    member_bridge.inbound({owner.device_id(), sent[1]});
    require(delivered.size() == 4, "disconnection revokes inbound packet injection");
    owner_bridge.clear();
    owner_bridge.outbound(plaintext);
    require(sent.size() == 5, "revoked network stops outbound packets");
}

void test_multicast() {
    using Membership = lanlink::network::MulticastMembership;
    auto now = Membership::Clock::time_point{};
    Membership membership;
    constexpr std::uint32_t group = 0xef010203U;
    constexpr std::uint32_t source = 0x0a400001U;
    const auto put = [](auto& bytes, std::size_t offset, std::uint32_t value, std::size_t count) {
        for (std::size_t index = 0; index < count; ++index)
            bytes[offset + index] = std::byte{static_cast<unsigned char>(value >> ((count - 1 - index) * 8U))};
    };
    const auto finish = [&](auto& bytes) {
        put(bytes, 2, static_cast<std::uint32_t>(bytes.size()), 2);
        bytes[22] = bytes[23] = std::byte{0};
        std::uint32_t sum = 0;
        for (std::size_t index = 20; index < bytes.size(); index += 2)
            sum += (std::to_integer<unsigned char>(bytes[index]) << 8U) |
                   std::to_integer<unsigned char>(bytes[index + 1]);
        while (sum >> 16U) sum = (sum & 0xffffU) + (sum >> 16U);
        put(bytes, 22, static_cast<std::uint16_t>(~sum), 2);
    };
    auto report = ipv4_packet(source, group);
    report.resize(28);
    report[8] = std::byte{1}; report[9] = std::byte{2}; report[20] = std::byte{0x16};
    put(report, 24, group, 4); finish(report);
    require(!membership.receives(group, source, now), "unjoined administrative multicast is filtered");
    require(membership.observe(report, now) == Membership::Report::accepted &&
            membership.receives(group, source, now), "IGMPv2 join enables this group");
    report[20] = std::byte{0x17}; put(report, 16, 0xe0000002U, 4); finish(report);
    require(membership.observe(report, now) == Membership::Report::accepted &&
            !membership.receives(group, source, now), "IGMPv2 leave revokes the group");
    report.resize(40);
    report[20] = std::byte{0x22}; put(report, 16, 0xe0000016U, 4);
    put(report, 24, 0, 2); put(report, 26, 1, 2);
    report[28] = std::byte{1}; report[29] = std::byte{0};
    put(report, 30, 1, 2); put(report, 32, group, 4); put(report, 36, source, 4); finish(report);
    require(membership.observe(report, now) == Membership::Report::accepted &&
            membership.receives(group, source, now) && !membership.receives(group, source + 1, now),
            "IGMPv3 INCLUDE filters sources");
    auto invalid = report; invalid[23] ^= std::byte{1};
    require(membership.observe(invalid, now) == Membership::Report::rejected &&
            membership.receives(group, source, now), "bad checksum cannot change membership");
    invalid = report; invalid[8] = std::byte{2};
    require(membership.observe(invalid, now) == Membership::Report::rejected,
            "IGMP must remain link local with TTL one");
    require(!membership.receives(group, source, now + std::chrono::seconds{261}) &&
            !membership.receives(0xe0010203U, source, now), "expired and global groups are filtered");
    membership.synchronize(std::vector<std::uint32_t>{group});
    require(membership.receives(group, source, now + std::chrono::hours{1}) &&
            !membership.receives(0xe00000fbU, source, now), "OS membership snapshot is authoritative");
    membership.synchronize(std::vector<std::uint32_t>{});
    require(!membership.receives(group, source, now), "OS group leave takes effect without IGMP timers");
}

}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("lanlink-packet-bridge-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(directory);
        test_multicast();
        test_bridge(directory);
        std::filesystem::remove_all(directory);
        std::cout << "all packet bridge tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        return 1;
    }
}
