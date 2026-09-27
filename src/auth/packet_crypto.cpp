#include "lanlink/auth/packet_crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace lanlink::auth {
namespace {

using DerivationContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

constexpr std::string_view key_domain = "lanlink-virtual-packet-key-v1";
constexpr std::string_view aad_domain = "lanlink-virtual-packet-aad-v1";
constexpr std::size_t wire_header_size = protocol::network_id_size + 4 + 8 + 4 +
                                         protocol::packet_nonce_size + 2;

template <std::size_t Size>
bool is_zero(const std::array<std::byte, Size>& value) {
    return std::all_of(value.begin(), value.end(),
                       [](const std::byte byte) { return byte == std::byte{0}; });
}

void append_u32(std::vector<std::byte>& output, const std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        output.push_back(std::byte{static_cast<unsigned char>(
            value >> ((3 - index) * 8U))});
    }
}

void append_u64(std::vector<std::byte>& output, const std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        output.push_back(std::byte{static_cast<unsigned char>(
            value >> ((7 - index) * 8U))});
    }
}

std::uint32_t read_ipv4(const std::span<const std::byte> packet,
                        const std::size_t offset) {
    std::uint32_t result = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        result = (result << 8U) |
                 std::to_integer<unsigned char>(packet[offset + index]);
    }
    return result;
}

std::pair<std::uint32_t, std::uint32_t> ipv4_addresses(
    const std::span<const std::byte> packet) {
    if (packet.size() < protocol::min_virtual_ipv4_packet_size ||
        packet.size() > protocol::max_virtual_ipv4_packet_size ||
        (std::to_integer<unsigned char>(packet[0]) >> 4U) != 4U) {
        throw std::invalid_argument("virtual packet has invalid IPv4 length or version");
    }
    const auto header_length =
        static_cast<std::size_t>(std::to_integer<unsigned char>(packet[0]) & 0x0fU) * 4U;
    const auto total_length = static_cast<std::size_t>(
        (std::to_integer<unsigned char>(packet[2]) << 8U) |
         std::to_integer<unsigned char>(packet[3]));
    if (header_length < protocol::min_virtual_ipv4_packet_size ||
        header_length > packet.size() || total_length != packet.size()) {
        throw std::invalid_argument("virtual packet has an invalid IPv4 header");
    }
    return {read_ipv4(packet, 12), read_ipv4(packet, 16)};
}

Secret32 derive_sender_key(const NetworkKey& network_key,
                           const protocol::NetworkId& network_id,
                           const std::uint64_t epoch,
                           const DeviceId& sender_id,
                           const EncryptionPublicKey& session_public_key) {
    std::vector<std::byte> salt(network_id.begin(), network_id.end());
    append_u64(salt, epoch);
    std::vector<std::byte> info;
    info.reserve(key_domain.size() + sender_id.size() + session_public_key.size());
    info.insert(info.end(),
                reinterpret_cast<const std::byte*>(key_domain.data()),
                reinterpret_cast<const std::byte*>(key_domain.data() + key_domain.size()));
    info.insert(info.end(), sender_id.begin(), sender_id.end());
    info.insert(info.end(), session_public_key.begin(), session_public_key.end());

    DerivationContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr),
                              EVP_PKEY_CTX_free);
    std::array<std::byte, 32> raw{};
    auto size = raw.size();
    if (!context || EVP_PKEY_derive_init(context.get()) != 1 ||
        EVP_PKEY_CTX_set_hkdf_md(context.get(), EVP_sha256()) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_salt(
            context.get(), reinterpret_cast<const unsigned char*>(salt.data()),
            static_cast<int>(salt.size())) != 1 ||
        EVP_PKEY_CTX_set1_hkdf_key(
            context.get(),
            reinterpret_cast<const unsigned char*>(network_key.bytes().data()),
            static_cast<int>(network_key.bytes().size())) != 1 ||
        EVP_PKEY_CTX_add1_hkdf_info(
            context.get(), reinterpret_cast<const unsigned char*>(info.data()),
            static_cast<int>(info.size())) != 1 ||
        EVP_PKEY_derive(context.get(),
                        reinterpret_cast<unsigned char*>(raw.data()), &size) != 1 ||
        size != raw.size()) {
        OPENSSL_cleanse(raw.data(), raw.size());
        throw std::runtime_error("virtual packet key derivation failed");
    }
    Secret32 result(raw);
    OPENSSL_cleanse(raw.data(), raw.size());
    return result;
}

std::vector<std::byte> authenticated_data(
    const protocol::EncryptedNetworkPacket& packet,
    const DeviceId& sender_id,
    const EncryptionPublicKey& session_public_key,
    const std::uint32_t source_ipv4) {
    auto header = protocol::encode_encrypted_network_packet(packet);
    header.resize(wire_header_size);
    std::vector<std::byte> output;
    output.reserve(aad_domain.size() + header.size() + sender_id.size() +
                   session_public_key.size() + 4);
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(aad_domain.data()),
                  reinterpret_cast<const std::byte*>(aad_domain.data() + aad_domain.size()));
    output.insert(output.end(), header.begin(), header.end());
    output.insert(output.end(), sender_id.begin(), sender_id.end());
    output.insert(output.end(), session_public_key.begin(), session_public_key.end());
    append_u32(output, source_ipv4);
    return output;
}

void encrypt_packet(const std::span<const std::byte> plaintext,
                    const Secret32& key,
                    const std::span<const std::byte> aad,
                    protocol::EncryptedNetworkPacket& packet) {
    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int used = 0;
    int extra = 0;
    if (!context || EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(),
                                       nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(packet.nonce.size()), nullptr) != 1 ||
        EVP_EncryptInit_ex(
            context.get(), nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(key.bytes().data()),
            reinterpret_cast<const unsigned char*>(packet.nonce.data())) != 1 ||
        EVP_EncryptUpdate(context.get(), nullptr, &extra,
                          reinterpret_cast<const unsigned char*>(aad.data()),
                          static_cast<int>(aad.size())) != 1 ||
        EVP_EncryptUpdate(
            context.get(), reinterpret_cast<unsigned char*>(packet.ciphertext.data()),
            &used, reinterpret_cast<const unsigned char*>(plaintext.data()),
            static_cast<int>(plaintext.size())) != 1 ||
        used != static_cast<int>(packet.ciphertext.size()) ||
        EVP_EncryptFinal_ex(
            context.get(),
            reinterpret_cast<unsigned char*>(packet.ciphertext.data()) + used,
            &extra) != 1 || extra != 0 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(packet.tag.size()),
                            packet.tag.data()) != 1) {
        throw std::runtime_error("virtual packet encryption failed");
    }
}

std::vector<std::byte> decrypt_packet(
    const protocol::EncryptedNetworkPacket& packet,
    const Secret32& key,
    const std::span<const std::byte> aad) {
    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    std::vector<std::byte> plaintext(packet.ciphertext.size());
    int used = 0;
    int extra = 0;
    if (!context || EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(),
                                       nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(packet.nonce.size()), nullptr) != 1 ||
        EVP_DecryptInit_ex(
            context.get(), nullptr, nullptr,
            reinterpret_cast<const unsigned char*>(key.bytes().data()),
            reinterpret_cast<const unsigned char*>(packet.nonce.data())) != 1 ||
        EVP_DecryptUpdate(context.get(), nullptr, &extra,
                          reinterpret_cast<const unsigned char*>(aad.data()),
                          static_cast<int>(aad.size())) != 1 ||
        EVP_DecryptUpdate(
            context.get(), reinterpret_cast<unsigned char*>(plaintext.data()),
            &used,
            reinterpret_cast<const unsigned char*>(packet.ciphertext.data()),
            static_cast<int>(packet.ciphertext.size())) != 1 ||
        used != static_cast<int>(plaintext.size()) ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG,
                            static_cast<int>(packet.tag.size()),
                            const_cast<std::byte*>(packet.tag.data())) != 1 ||
        EVP_DecryptFinal_ex(
            context.get(), reinterpret_cast<unsigned char*>(plaintext.data()) + used,
            &extra) != 1 || extra != 0) {
        OPENSSL_cleanse(plaintext.data(), plaintext.size());
        throw std::runtime_error("virtual packet authentication failed");
    }
    return plaintext;
}

bool accept_sequence(const std::uint32_t sequence,
                     const std::uint32_t highest,
                     const std::uint64_t bitmap) {
    if (sequence > highest) {
        return true;
    }
    const auto distance = highest - sequence;
    return distance < 64 && (bitmap & (std::uint64_t{1} << distance)) == 0;
}

}

NetworkPacketCipher::NetworkPacketCipher(
    protocol::NetworkId network_id,
    const std::uint64_t key_epoch,
    NetworkKey network_key,
    DeviceId own_device_id,
    const SignedDeviceKey& own_signed_key,
    const std::uint32_t own_ipv4_address)
    : network_id_(network_id),
      key_epoch_(key_epoch),
      network_key_(std::move(network_key)),
      own_device_id_(own_device_id),
      own_session_public_key_(own_signed_key.encryption_public_key),
      own_ipv4_address_(own_ipv4_address),
      outgoing_key_(derive_sender_key(network_key_, network_id_, key_epoch_,
                                       own_device_id_, own_session_public_key_)) {
    if (is_zero(network_id_) || key_epoch_ == 0 ||
        !verify_signed_device_key(own_signed_key, own_device_id_) ||
        own_ipv4_address_ == 0 || own_ipv4_address_ >= 0xe0000000U) {
        throw std::invalid_argument("virtual packet sender is invalid");
    }
    if (RAND_bytes(reinterpret_cast<unsigned char*>(outgoing_nonce_prefix_.data()),
                   static_cast<int>(outgoing_nonce_prefix_.size())) != 1) {
        throw std::runtime_error("virtual packet nonce generation failed");
    }
}

void NetworkPacketCipher::update_peers(const protocol::NetworkPeerState& state) {
    if (state.network_id != network_id_ || state.key_epoch != key_epoch_ ||
        state.own_address != own_ipv4_address_) {
        throw std::invalid_argument("virtual packet peer state does not match network key");
    }
    static_cast<void>(protocol::encode_network_peer_state(state));

    std::lock_guard lock(mutex_);
    std::map<DeviceId, PeerReceiver> updated;
    for (const auto& peer : state.peers) {
        if (!peer.signed_key) {
            continue;
        }
        if (peer.device_id == own_device_id_ ||
            !verify_signed_device_key(*peer.signed_key, peer.device_id)) {
            throw std::invalid_argument("virtual packet peer has an invalid signed key");
        }
        const auto& session_public_key = peer.signed_key->encryption_public_key;
        const auto previous = peers_.find(peer.device_id);
        if (previous != peers_.end() &&
            previous->second.session_public_key == session_public_key &&
            previous->second.ipv4_address == peer.ipv4_address) {
            updated.emplace(peer.device_id, previous->second);
        } else {
            updated.emplace(peer.device_id, PeerReceiver{
                session_public_key, peer.ipv4_address,
                derive_sender_key(network_key_, network_id_, key_epoch_,
                                  peer.device_id, session_public_key)});
        }
    }
    peers_.swap(updated);
}

protocol::EncryptedNetworkPacket NetworkPacketCipher::encrypt(
    const std::span<const std::byte> ipv4_packet) {
    const auto [source, destination] = ipv4_addresses(ipv4_packet);
    if (source != own_ipv4_address_ || destination == 0 ||
        destination >= 0xe0000000U || destination == own_ipv4_address_) {
        throw std::invalid_argument("virtual packet has an invalid source or destination");
    }

    std::lock_guard lock(mutex_);
    if (next_sequence_ == std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("virtual packet sequence exhausted");
    }
    protocol::EncryptedNetworkPacket packet;
    packet.network_id = network_id_;
    packet.destination_ipv4 = destination;
    packet.key_epoch = key_epoch_;
    packet.sequence = ++next_sequence_;
    std::copy(outgoing_nonce_prefix_.begin(), outgoing_nonce_prefix_.end(),
              packet.nonce.begin());
    for (std::size_t index = 0; index < 4; ++index) {
        packet.nonce[8 + index] = std::byte{static_cast<unsigned char>(
            packet.sequence >> ((3 - index) * 8U))};
    }
    packet.ciphertext.resize(ipv4_packet.size());
    const auto aad = authenticated_data(packet, own_device_id_,
                                        own_session_public_key_, own_ipv4_address_);
    encrypt_packet(ipv4_packet, outgoing_key_, aad, packet);
    return packet;
}

std::vector<std::byte> NetworkPacketCipher::decrypt(
    const protocol::EncryptedNetworkPacket& packet,
    const DeviceId& authenticated_sender) {
    static_cast<void>(protocol::encode_encrypted_network_packet(packet));
    if (packet.network_id != network_id_ || packet.key_epoch != key_epoch_ ||
        packet.destination_ipv4 != own_ipv4_address_) {
        throw std::runtime_error("virtual packet does not match current network key");
    }

    std::lock_guard lock(mutex_);
    const auto peer = peers_.find(authenticated_sender);
    if (peer == peers_.end()) {
        throw std::runtime_error("virtual packet sender is not an active peer");
    }
    auto& receiver = peer->second;
    if (receiver.initialized &&
        (!std::equal(receiver.nonce_prefix.begin(), receiver.nonce_prefix.end(),
                     packet.nonce.begin()) ||
         !accept_sequence(packet.sequence, receiver.highest_sequence,
                          receiver.seen_sequences))) {
        throw std::runtime_error("virtual packet replay or sender nonce change");
    }

    const auto aad = authenticated_data(packet, authenticated_sender,
                                        receiver.session_public_key,
                                        receiver.ipv4_address);
    auto plaintext = decrypt_packet(packet, receiver.encryption_key, aad);
    bool addresses_match = false;
    try {
        const auto [source, destination] = ipv4_addresses(plaintext);
        addresses_match = source == receiver.ipv4_address &&
                          destination == own_ipv4_address_;
    } catch (const std::invalid_argument&) {
    }
    if (!addresses_match) {
        OPENSSL_cleanse(plaintext.data(), plaintext.size());
        throw std::runtime_error("decrypted virtual packet addresses are invalid");
    }

    if (!receiver.initialized) {
        std::copy_n(packet.nonce.begin(), receiver.nonce_prefix.size(),
                    receiver.nonce_prefix.begin());
        receiver.highest_sequence = packet.sequence;
        receiver.seen_sequences = 1;
        receiver.initialized = true;
    } else if (packet.sequence > receiver.highest_sequence) {
        const auto distance = packet.sequence - receiver.highest_sequence;
        receiver.seen_sequences = distance >= 64 ? 1 :
                                  (receiver.seen_sequences << distance) | 1;
        receiver.highest_sequence = packet.sequence;
    } else {
        receiver.seen_sequences |=
            std::uint64_t{1} << (receiver.highest_sequence - packet.sequence);
    }
    return plaintext;
}

}
