#include "lanlink/protocol/packet_messages.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>

int main() {
    namespace p = lanlink::protocol;
    constexpr unsigned iterations = 10000;
    std::uint64_t checksum = 0;
    std::cout << "payload_bytes,iterations,full_copy_median_ns,header_only_median_ns,avoided_copy_bytes_per_encrypt_decrypt\n";
    for (const std::size_t size : {24U, 1400U, 65535U}) {
        p::EncryptedNetworkPacket packet;
        packet.network_id[0] = std::byte{1};
        packet.destination_ipv4 = 0x0a400002;
        packet.key_epoch = 0x0102030405060708ULL;
        packet.sequence = 0x12345678;
        packet.nonce[8] = std::byte{0x12}; packet.nonce[9] = std::byte{0x34};
        packet.nonce[10] = std::byte{0x56}; packet.nonce[11] = std::byte{0x78};
        packet.ciphertext.resize(size, std::byte{0x7b});
        const auto wire = p::encode_encrypted_network_packet(packet);
        const auto header = p::encode_encrypted_network_packet_header(packet);
        if (!std::equal(header.begin(), header.end(), wire.begin()) ||
            header[20] != std::byte{1} || header[27] != std::byte{8} ||
            header[28] != std::byte{0x12} || header[31] != std::byte{0x78})
            throw std::runtime_error("header wire format changed");
        std::array<double, 5> full{}, compact{};
        for (unsigned sample = 0; sample < full.size(); ++sample) {
            const auto measure = [&](bool copy) {
                const auto begin = std::chrono::steady_clock::now();
                for (unsigned index = 0; index < iterations; ++index) {
                    if (copy) {
                        auto encoded = p::encode_encrypted_network_packet(packet);
                        encoded.resize(p::encrypted_packet_header_size);
                        checksum += std::to_integer<unsigned>(encoded[index % encoded.size()]);
                    } else {
                        const auto encoded = p::encode_encrypted_network_packet_header(packet);
                        checksum += std::to_integer<unsigned>(encoded[index % encoded.size()]);
                    }
                }
                return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count() / iterations;
            };
            if (sample % 2) { compact[sample] = measure(false); full[sample] = measure(true); }
            else { full[sample] = measure(true); compact[sample] = measure(false); }
        }
        std::sort(full.begin(), full.end()); std::sort(compact.begin(), compact.end());
        std::cout << size << ',' << iterations << ',' << full[2] << ',' << compact[2] << ','
                  << 3 * (size + p::packet_tag_size) << '\n';
    }
    std::cerr << "benchmark checksum=" << checksum << '\n';
}
