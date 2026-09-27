#include "lanlink/auth/handshake.hpp"
#include "lanlink/protocol/packet_messages.hpp"
#include "lanlink/relay/network_control_channel.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace auth = lanlink::auth;
namespace protocol = lanlink::protocol;
namespace relay = lanlink::relay;
namespace storage = lanlink::storage;

void expect(const bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}

template <typename Function>
void expect_error(Function&& action, const char* reason) {
    try {
        action();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(reason);
}

void authenticate(auth::ClientHandshake& client, auth::ServerHandshake& server) {
    const auto challenge = server.handle_hello(client.begin(1));
    const auto accepted = server.handle_proof(client.handle_challenge(challenge));
    expect(client.handle_result(accepted), "device handshake must authenticate");
}

void test_routing(const std::filesystem::path& directory) {
    const auto owner = auth::DeviceIdentity::load_or_create(directory / "owner.identity");
    const auto member = auth::DeviceIdentity::load_or_create(directory / "member.identity");
    const auto outsider = auth::DeviceIdentity::load_or_create(directory / "outsider.identity");
    const auto token = auth::AuthToken::from_secret(
        "lanlink-encrypted-routing-integration-test-secret");
    auth::ClientHandshake owner_client(owner, token);
    auth::ClientHandshake member_client(member, token);
    auth::ClientHandshake outsider_client(outsider, token);
    const auto deadline = auth::ServerHandshake::Clock::now() +
                          std::chrono::minutes{1};
    auth::ServerHandshake owner_server(token, 301, deadline);
    auth::ServerHandshake member_server(token, 302, deadline);
    auth::ServerHandshake outsider_server(token, 303, deadline);
    authenticate(owner_client, owner_server);
    authenticate(member_client, member_server);
    authenticate(outsider_client, outsider_server);

    protocol::NetworkId first{};
    protocol::NetworkId second{};
    first.front() = std::byte{0x31};
    second.front() = std::byte{0x32};
    std::vector<protocol::NetworkId> generated{first, second};
    std::size_t generated_index = 0;
    storage::RelayStore store(directory / "relay.db");
    relay::NetworkService service(store, [&] {
        return generated.at(generated_index++);
    });
    relay::NetworkControlChannel channel(service);
    static_cast<void>(service.publish_device_key(
        owner.device_id(), owner_server.session_id(),
        owner_server.signed_device_key(), 1));
    static_cast<void>(service.publish_device_key(
        member.device_id(), member_server.session_id(),
        member_server.signed_device_key(), 1));
    static_cast<void>(service.publish_device_key(
        outsider.device_id(), outsider_server.session_id(),
        outsider_server.signed_device_key(), 1));
    expect(service.create_network(owner.device_id(), {"First"}, 2).result.code ==
               protocol::NetworkResultCode::success &&
           service.create_network(owner.device_id(), {"Second"}, 3).result.code ==
               protocol::NetworkResultCode::success,
           "create two isolated networks");
    expect(service.invite_member(owner.device_id(), {first, member.device_id()}, 4)
                   .result.code == protocol::NetworkResultCode::success &&
           service.join_network(member.device_id(), {first}, 5).result.code ==
               protocol::NetworkResultCode::success &&
           service.invite_member(owner.device_id(), {second, outsider.device_id()}, 6)
                   .result.code == protocol::NetworkResultCode::success &&
           service.join_network(outsider.device_id(), {second}, 7).result.code ==
               protocol::NetworkResultCode::success,
           "each network grants a separate virtual IPv4 lease");
    const auto owner_ip = store.find_virtual_ipv4_lease(first, owner.device_id())->address;
    const auto member_ip = store.find_virtual_ipv4_lease(first, member.device_id())->address;

    protocol::EncryptedNetworkPacket packet;
    packet.network_id = first;
    packet.destination_ipv4 = member_ip;
    packet.key_epoch = 2;
    packet.sequence = 1;
    packet.nonce.back() = std::byte{1};
    packet.ciphertext.assign(24, std::byte{0x61});
    packet.tag.fill(std::byte{0x79});
    const auto payload = protocol::encode_encrypted_network_packet(packet);
    const auto routed = channel.route_encrypted_packet(owner.device_id(), payload);
    expect(routed && routed->recipient_device_id == member.device_id() &&
               routed->delivery.sender_device_id == owner.device_id() &&
               routed->delivery.packet == packet,
           "authenticated sender's ciphertext goes to the leased recipient unchanged");
    const auto delivered = protocol::encode_forwarded_network_packet(routed->delivery);
    expect(protocol::decode_forwarded_network_packet(delivered) == routed->delivery,
           "forwarded packet carries authenticated sender through the wire codec");

    expect(!service.route_encrypted_packet(outsider.device_id(), packet),
           "member of another network cannot claim the sender's network");
    auto altered = packet;
    altered.network_id = second;
    expect(!service.route_encrypted_packet(owner.device_id(), altered),
           "same sender cannot route to a virtual address in a different network");
    altered = packet;
    altered.destination_ipv4 = owner_ip;
    expect(!service.route_encrypted_packet(owner.device_id(), altered),
           "sender cannot route packets back to its own address");
    altered.destination_ipv4 = member_ip + 10;
    expect(!service.route_encrypted_packet(owner.device_id(), altered),
           "unleased destination is not forwarded");
    altered = packet;
    ++altered.key_epoch;
    expect(!service.route_encrypted_packet(owner.device_id(), altered),
           "stale or unpublished key generation cannot be routed");

    altered = packet;
    altered.destination_ipv4 = owner_ip;
    altered.sequence = 2;
    altered.nonce.back() = std::byte{2};
    expect(service.route_encrypted_packet(member.device_id(), altered)
                   ->recipient_device_id == owner.device_id(),
           "member can reply using its own authenticated session");

    static_cast<void>(service.remove_device_key(member.device_id(),
                                                member_server.session_id()));
    expect(!service.route_encrypted_packet(owner.device_id(), packet),
           "disconnected destination is not forwarded packets");
    static_cast<void>(service.publish_device_key(
        member.device_id(), member_server.session_id(),
        member_server.signed_device_key(), 8));
    expect(service.route_encrypted_packet(owner.device_id(), packet).has_value(),
           "reconnected network member receives packets again");
    expect(service.kick_member(owner.device_id(), {first, member.device_id()}, 9)
                   .result.code == protocol::NetworkResultCode::success &&
               !service.route_encrypted_packet(owner.device_id(), packet),
           "revocation removes the lease and rejects earlier key generations");

    auto truncated = payload;
    truncated.pop_back();
    expect_error([&] {
        static_cast<void>(channel.route_encrypted_packet(owner.device_id(), truncated));
    }, "malformed ciphertext is rejected before routing");
    auto invalid_delivery = routed->delivery;
    invalid_delivery.sender_device_id.fill(std::byte{0});
    expect_error([&] {
        static_cast<void>(protocol::encode_forwarded_network_packet(invalid_delivery));
    }, "forwarded packet cannot omit authenticated sender identity");
}

}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("lanlink-packet-routing-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    try {
        test_routing(directory);
        std::filesystem::remove_all(directory);
        std::cout << "all packet routing tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "packet routing test failed: " << error.what() << '\n';
        std::filesystem::remove_all(directory);
        return 1;
    }
}
