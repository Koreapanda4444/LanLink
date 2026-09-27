#include "lanlink/auth/handshake.hpp"
#include "lanlink/auth/packet_crypto.hpp"
#include "lanlink/protocol/packet_messages.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

namespace auth = lanlink::auth;
namespace protocol = lanlink::protocol;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "failed: " << message << '\n';
        ++failures;
    }
}

template <typename Function>
void expect_error(Function&& function, const std::string_view message) {
    try {
        function();
        expect(false, message);
    } catch (const std::exception&) {
    }
}

void authenticate(auth::ClientHandshake& client, auth::ServerHandshake& server) {
    const auto challenge = server.handle_hello(client.begin(1));
    const auto result = server.handle_proof(client.handle_challenge(challenge));
    if (!client.handle_result(result) || !server.authenticated()) {
        throw std::runtime_error("packet test authentication failed");
    }
}

std::vector<std::byte> ipv4_packet(const std::uint32_t source,
                                   const std::uint32_t destination) {
    std::vector<std::byte> packet(24);
    packet[0] = std::byte{0x45};
    packet[2] = std::byte{0};
    packet[3] = std::byte{24};
    packet[8] = std::byte{64};
    packet[9] = std::byte{17};
    for (std::size_t index = 0; index < 4; ++index) {
        packet[12 + index] = std::byte{static_cast<unsigned char>(
            source >> ((3 - index) * 8U))};
        packet[16 + index] = std::byte{static_cast<unsigned char>(
            destination >> ((3 - index) * 8U))};
    }
    packet[20] = std::byte{0x89};
    packet[21] = std::byte{0x5a};
    packet[22] = std::byte{0xc3};
    packet[23] = std::byte{0xf1};
    return packet;
}

void test_packet_crypto(const std::filesystem::path& directory) {
    const auto owner = auth::DeviceIdentity::load_or_create(directory / "packet-owner.identity");
    const auto member = auth::DeviceIdentity::load_or_create(directory / "packet-member.identity");
    const auto outsider = auth::DeviceIdentity::load_or_create(directory / "packet-outsider.identity");
    const auto token = auth::AuthToken::from_secret(
        "lanlink-encrypted-virtual-packet-test-secret");
    auth::ClientHandshake owner_client(owner, token);
    auth::ClientHandshake member_client(member, token);
    auth::ServerHandshake owner_server(
        token, 401, auth::ServerHandshake::Clock::now() + std::chrono::minutes{1});
    auth::ServerHandshake member_server(
        token, 402, auth::ServerHandshake::Clock::now() + std::chrono::minutes{1});
    authenticate(owner_client, owner_server);
    authenticate(member_client, member_server);

    protocol::NetworkId network_id{};
    network_id.front() = std::byte{0x13};
    constexpr std::uint32_t owner_ip = 0x0a400001U;
    constexpr std::uint32_t member_ip = 0x0a400002U;
    const auto key = auth::NetworkKey::random();
    auth::NetworkPacketCipher owner_cipher(
        network_id, 5, key, owner.device_id(), owner_server.signed_device_key(), owner_ip);
    auth::NetworkPacketCipher member_cipher(
        network_id, 5, key, member.device_id(), member_server.signed_device_key(), member_ip);

    protocol::NetworkPeerState owner_state{
        network_id, 1, 0x0a400000U, 24, owner_ip,
        {{member.device_id(), member_ip, member_server.signed_device_key()}},
        owner.device_id(), 5};
    protocol::NetworkPeerState member_state{
        network_id, 1, 0x0a400000U, 24, member_ip,
        {{owner.device_id(), owner_ip, owner_server.signed_device_key()}},
        owner.device_id(), 5};
    owner_cipher.update_peers(owner_state);
    member_cipher.update_peers(member_state);

    const auto plaintext = ipv4_packet(owner_ip, member_ip);
    const auto encrypted = owner_cipher.encrypt(plaintext);
    expect(encrypted.network_id == network_id && encrypted.key_epoch == 5 &&
               encrypted.destination_ipv4 == member_ip && encrypted.sequence == 1 &&
               encrypted.ciphertext != plaintext,
           "virtual IPv4 packet is encrypted for the active network and destination");

    const auto wire = protocol::encode_encrypted_network_packet(encrypted);
    expect(wire.size() == 62 + plaintext.size() && wire[16] == std::byte{0x0a} &&
               wire[17] == std::byte{0x40} && wire[19] == std::byte{0x02},
           "encrypted packet wire exposes only routing and cipher metadata");
    const auto decoded = protocol::decode_encrypted_network_packet(wire);
    expect(decoded == encrypted &&
               member_cipher.decrypt(decoded, owner.device_id()) == plaintext,
           "recipient decrypts the complete IPv4 packet after wire round trip");
    expect_error([&] {
        static_cast<void>(member_cipher.decrypt(encrypted, owner.device_id()));
    }, "recipient rejects duplicate ciphertext");
    member_cipher.update_peers(member_state);
    expect_error([&] {
        static_cast<void>(member_cipher.decrypt(encrypted, owner.device_id()));
    }, "unchanged peer snapshot preserves replay history");

    const auto second = owner_cipher.encrypt(plaintext);
    auto changed = second;
    changed.ciphertext.front() ^= std::byte{1};
    expect_error([&] {
        static_cast<void>(member_cipher.decrypt(changed, owner.device_id()));
    }, "altered ciphertext rejected without marking packet as received");
    changed = second;
    changed.tag.front() ^= std::byte{1};
    expect_error([&] {
        static_cast<void>(member_cipher.decrypt(changed, owner.device_id()));
    }, "altered authentication tag rejected");
    changed = second;
    changed.nonce.front() ^= std::byte{1};
    expect_error([&] {
        static_cast<void>(member_cipher.decrypt(changed, owner.device_id()));
    }, "altered sender nonce rejected");
    changed = second;
    changed.sequence += 1;
    changed.nonce.back() = std::byte{3};
    expect_error([&] {
        static_cast<void>(member_cipher.decrypt(changed, owner.device_id()));
    }, "altered authenticated sequence rejected");
    changed = second;
    changed.destination_ipv4 = owner_ip;
    expect_error([&] {
        static_cast<void>(member_cipher.decrypt(changed, owner.device_id()));
    }, "packet addressed to another device is rejected");
    changed = second;
    changed.key_epoch = 6;
    expect_error([&] {
        static_cast<void>(member_cipher.decrypt(changed, owner.device_id()));
    }, "stale or incorrect key epoch is rejected");
    changed = second;
    changed.network_id.front() ^= std::byte{1};
    expect_error([&] {
        static_cast<void>(member_cipher.decrypt(changed, owner.device_id()));
    }, "cross-network ciphertext is rejected");
    expect_error([&] {
        static_cast<void>(member_cipher.decrypt(second, outsider.device_id()));
    }, "unregistered sender cannot decrypt as a current peer");
    expect(member_cipher.decrypt(second, owner.device_id()) == plaintext,
           "failed integrity checks do not poison the replay window");

    auth::NetworkPacketCipher wrong_key_cipher(
        network_id, 5, auth::NetworkKey::random(), member.device_id(),
        member_server.signed_device_key(), member_ip);
    wrong_key_cipher.update_peers(member_state);
    expect_error([&] {
        static_cast<void>(wrong_key_cipher.decrypt(second, owner.device_id()));
    }, "a different group key cannot authenticate ciphertext at the same epoch");
    auth::NetworkPacketCipher wrong_address_cipher(
        network_id, 5, key, owner.device_id(),
        owner_server.signed_device_key(), 0x0a400003U);
    expect_error([&] {
        static_cast<void>(member_cipher.decrypt(
            wrong_address_cipher.encrypt(ipv4_packet(0x0a400003U, member_ip)),
            owner.device_id()));
    }, "packet authentication binds the sender's allocated virtual address");

    const auto reverse_plaintext = ipv4_packet(member_ip, owner_ip);
    const auto reverse = member_cipher.encrypt(reverse_plaintext);
    expect(owner_cipher.decrypt(reverse, member.device_id()) == reverse_plaintext,
           "both network members use distinct authenticated sending sessions");

    std::vector<protocol::EncryptedNetworkPacket> packets;
    for (std::uint32_t sequence = 3; sequence <= 74; ++sequence) {
        packets.push_back(owner_cipher.encrypt(plaintext));
        expect(packets.back().sequence == sequence,
               "sender sequences are monotonically increasing");
    }
    const auto at = [&](const std::uint32_t sequence) -> const auto& {
        return packets[sequence - 3];
    };
    expect(member_cipher.decrypt(at(74), owner.device_id()) == plaintext &&
               member_cipher.decrypt(at(73), owner.device_id()) == plaintext &&
               member_cipher.decrypt(at(11), owner.device_id()) == plaintext,
           "out-of-order packets within the 64-sequence window are accepted");
    expect_error([&] {
        static_cast<void>(member_cipher.decrypt(at(11), owner.device_id()));
    }, "out-of-order duplicate is rejected");
    expect_error([&] {
        static_cast<void>(member_cipher.decrypt(at(10), owner.device_id()));
    }, "packet beyond the 64-sequence replay window is rejected");

    auto bad_state = member_state;
    bad_state.peers.front().signed_key->signature.front() ^= std::byte{1};
    expect_error([&] { member_cipher.update_peers(bad_state); },
                 "invalid authenticated peer key cannot replace replay state");
    expect(member_cipher.decrypt(at(72), owner.device_id()) == plaintext,
           "peer validation failure preserves the previous active peer");
    auto rotated_state = member_state;
    rotated_state.key_epoch++;
    expect_error([&] { member_cipher.update_peers(rotated_state); },
                 "new key epoch requires a new packet cipher");
    auth::NetworkPacketCipher rotated_cipher(
        network_id, 6, auth::NetworkKey::random(), member.device_id(),
        member_server.signed_device_key(), member_ip);
    rotated_cipher.update_peers(rotated_state);
    expect_error([&] {
        static_cast<void>(rotated_cipher.decrypt(encrypted, owner.device_id()));
    }, "rotated key no longer accepts the previous epoch's packets");

    auto invalid_ipv4 = plaintext;
    invalid_ipv4[0] = std::byte{0x65};
    expect_error([&] { static_cast<void>(owner_cipher.encrypt(invalid_ipv4)); },
                 "non-IPv4 payload cannot be sent");
    invalid_ipv4 = plaintext;
    invalid_ipv4[3] = std::byte{23};
    expect_error([&] { static_cast<void>(owner_cipher.encrypt(invalid_ipv4)); },
                 "mismatched IPv4 total length cannot be sent");
    invalid_ipv4 = ipv4_packet(member_ip, owner_ip);
    expect_error([&] { static_cast<void>(owner_cipher.encrypt(invalid_ipv4)); },
                 "sender cannot encrypt packets with a different source address");

    auto short_wire = wire;
    short_wire.pop_back();
    expect_error([&] {
        static_cast<void>(protocol::decode_encrypted_network_packet(short_wire));
    }, "truncated encrypted packet is rejected before decryption");
    auto extra_wire = wire;
    extra_wire.push_back(std::byte{0});
    expect_error([&] {
        static_cast<void>(protocol::decode_encrypted_network_packet(extra_wire));
    }, "trailing encrypted packet bytes are rejected");
    auto invalid_wire = wire;
    invalid_wire[45] = std::byte{23};
    expect_error([&] {
        static_cast<void>(protocol::decode_encrypted_network_packet(invalid_wire));
    }, "encrypted packet ciphertext length must match wire length");
    changed = encrypted;
    changed.nonce.back() = std::byte{9};
    expect_error([&] {
        static_cast<void>(protocol::encode_encrypted_network_packet(changed));
    }, "sequence and nonce must agree");
    changed = encrypted;
    changed.ciphertext.resize(protocol::max_virtual_ipv4_packet_size + 1);
    expect_error([&] {
        static_cast<void>(protocol::encode_encrypted_network_packet(changed));
    }, "oversized packet is rejected before encoding");
}

}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("lanlink-packet-crypto-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    try {
        test_packet_crypto(directory);
    } catch (const std::exception& error) {
        std::cerr << "unexpected error: " << error.what() << '\n';
        ++failures;
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    if (error) {
        std::cerr << "cleanup failed: " << error.message() << '\n';
        ++failures;
    }
    if (failures != 0) {
        std::cerr << failures << " packet crypto test failure(s)\n";
        return 1;
    }
    std::cout << "all packet crypto tests passed\n";
    return 0;
}
