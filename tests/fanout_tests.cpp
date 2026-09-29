#include "lanlink/auth/handshake.hpp"
#include "lanlink/auth/packet_crypto.hpp"
#include "lanlink/relay/network_service.hpp"

#include <array>
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
namespace relay = lanlink::relay;
namespace storage = lanlink::storage;

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void authenticate(auth::ClientHandshake& client, auth::ServerHandshake& server) {
    const auto result = server.handle_proof(
        client.handle_challenge(server.handle_hello(client.begin(1))));
    require(client.handle_result(result), "fanout device authentication failed");
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

void test_fanout(const std::filesystem::path& directory) {
    const auto owner = auth::DeviceIdentity::load_or_create(directory / "owner.identity");
    const auto first = auth::DeviceIdentity::load_or_create(directory / "first.identity");
    const auto second = auth::DeviceIdentity::load_or_create(directory / "second.identity");
    const auto outsider = auth::DeviceIdentity::load_or_create(directory / "outsider.identity");
    const auto token = auth::AuthToken::from_secret(
        "lanlink-broadcast-multicast-routing-test-token");
    auth::ClientHandshake owner_client(owner, token);
    auth::ClientHandshake first_client(first, token);
    auth::ClientHandshake second_client(second, token);
    auth::ClientHandshake outsider_client(outsider, token);
    const auto deadline = auth::ServerHandshake::Clock::now() + std::chrono::minutes{1};
    auth::ServerHandshake owner_server(token, 1, deadline);
    auth::ServerHandshake first_server(token, 2, deadline);
    auth::ServerHandshake second_server(token, 3, deadline);
    auth::ServerHandshake outsider_server(token, 4, deadline);
    authenticate(owner_client, owner_server);
    authenticate(first_client, first_server);
    authenticate(second_client, second_server);
    authenticate(outsider_client, outsider_server);

    protocol::NetworkId network{};
    protocol::NetworkId other{};
    network.front() = std::byte{1};
    other.front() = std::byte{2};
    std::size_t next_id = 0;
    auto now = relay::NetworkService::Clock::time_point{};
    storage::RelayStore store(directory / "relay.db");
    relay::NetworkService service(store,
        [&] { return next_id++ == 0 ? network : other; },
        [&] { return now; });
    const auto add_device = [&](const auto& identity, const auto& server) {
        static_cast<void>(service.publish_device_key(
            identity.device_id(), server.session_id(), server.signed_device_key(), 1));
    };
    add_device(owner, owner_server);
    add_device(first, first_server);
    add_device(second, second_server);
    add_device(outsider, outsider_server);

    require(service.create_network(owner.device_id(), {"LAN"}, 2).result.code ==
            protocol::NetworkResultCode::success, "create fanout network");
    const auto join = [&](const auto& identity) {
        require(service.invite_member(owner.device_id(),
                    {network, identity.device_id()}, 3).result.code ==
                    protocol::NetworkResultCode::success &&
                service.join_network(identity.device_id(), {network}, 4).result.code ==
                    protocol::NetworkResultCode::success,
                "join fanout network");
    };
    join(first);
    join(second);
    require(service.create_network(outsider.device_id(), {"Other"}, 5).result.code ==
            protocol::NetworkResultCode::success, "create isolated network");

    const auto owner_state = *service.peer_state(owner.device_id(), {network}, 6).state;
    const auto first_state = *service.peer_state(first.device_id(), {network}, 6).state;
    const auto second_state = *service.peer_state(second.device_id(), {network}, 6).state;
    const auto key = auth::NetworkKey::random();
    auth::NetworkPacketCipher owner_cipher(network, owner_state.key_epoch, key,
        owner.device_id(), owner_server.signed_device_key(), owner_state.own_address);
    auth::NetworkPacketCipher first_cipher(network, first_state.key_epoch, key,
        first.device_id(), first_server.signed_device_key(), first_state.own_address);
    auth::NetworkPacketCipher second_cipher(network, second_state.key_epoch, key,
        second.device_id(), second_server.signed_device_key(), second_state.own_address);
    owner_cipher.update_peers(owner_state);
    first_cipher.update_peers(first_state);
    second_cipher.update_peers(second_state);

    const auto broadcast = owner_state.subnet_address | 0xffU;
    constexpr std::array destinations{
        0xffffffffU, 0xe00000fbU, 0xeffffffaU};
    for (const auto destination : destinations) {
        const auto plaintext = ipv4_packet(owner_state.own_address, destination);
        const auto encrypted = owner_cipher.encrypt(plaintext);
        const auto deliveries = service.route_encrypted_packet(owner.device_id(), encrypted);
        require(deliveries.size() == 2 &&
                deliveries[0].recipient_device_id != owner.device_id() &&
                deliveries[1].recipient_device_id != owner.device_id() &&
                deliveries[0].recipient_device_id != deliveries[1].recipient_device_id &&
                deliveries[0].delivery.packet == encrypted,
                "same ciphertext reaches each active member exactly once");
        require(first_cipher.decrypt(encrypted, owner.device_id()) == plaintext &&
                second_cipher.decrypt(encrypted, owner.device_id()) == plaintext,
                "each member authenticates the original broadcast or multicast address");
    }
    const auto directed = ipv4_packet(owner_state.own_address, broadcast);
    const auto directed_packet = owner_cipher.encrypt(directed);
    require(service.route_encrypted_packet(owner.device_id(), directed_packet).size() == 2,
            "directed subnet broadcast is replicated");
    require(first_cipher.decrypt(directed_packet, owner.device_id()) == directed,
            "directed broadcast keeps its IPv4 destination");

    auto tampered = directed_packet;
    tampered.destination_ipv4 = 0xffffffffU;
    try {
        static_cast<void>(second_cipher.decrypt(tampered, owner.device_id()));
        throw std::runtime_error("altered fanout destination was accepted");
    } catch (const std::runtime_error& error) {
        require(std::string_view{error.what()} != "altered fanout destination was accepted",
                "fanout destination must be authenticated");
    }
    tampered = directed_packet;
    tampered.destination_ipv4 = 0x0a8000ffU;
    require(service.route_encrypted_packet(owner.device_id(), tampered).empty(),
            "other subnet broadcast is not replicated");
    tampered = directed_packet;
    tampered.ciphertext.resize(1401);
    require(service.route_encrypted_packet(owner.device_id(), tampered).empty(),
            "oversized fanout is discarded before replication");
    require(service.route_encrypted_packet(outsider.device_id(), directed_packet).empty(),
            "another network cannot relay this broadcast");

    for (int index = 0; index < 12; ++index) {
        require(service.route_encrypted_packet(owner.device_id(),
            owner_cipher.encrypt(directed)).size() == 2,
            "fanout stays within the initial burst allowance");
    }
    require(service.route_encrypted_packet(owner.device_id(),
        owner_cipher.encrypt(directed)).empty(),
        "fanout burst limit stops a packet storm");
    now += std::chrono::seconds{1};
    require(service.route_encrypted_packet(owner.device_id(),
        owner_cipher.encrypt(directed)).size() == 2,
        "fanout budget refills after idle time");
    static_cast<void>(service.remove_device_key(second.device_id(),
                                                second_server.session_id()));
    const auto remaining = service.route_encrypted_packet(owner.device_id(),
        owner_cipher.encrypt(directed));
    require(remaining.size() == 1 && remaining.front().recipient_device_id ==
            first.device_id(), "disconnected peer is removed from broadcast recipients");
}

}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("lanlink-fanout-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(directory);
        test_fanout(directory);
        std::filesystem::remove_all(directory);
        std::cout << "all fanout tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        return 1;
    }
}
