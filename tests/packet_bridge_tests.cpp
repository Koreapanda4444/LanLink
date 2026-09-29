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

    member_bridge.synchronize(std::nullopt, {});
    member_bridge.inbound({owner.device_id(), sent[1]});
    require(delivered.size() == 2, "disconnection revokes inbound packet injection");
    owner_bridge.clear();
    owner_bridge.outbound(plaintext);
    require(sent.size() == 3, "revoked network stops outbound packets");
}

}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("lanlink-packet-bridge-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(directory);
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
