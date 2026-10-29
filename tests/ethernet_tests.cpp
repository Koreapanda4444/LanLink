#include "lanlink/network/ethernet.hpp"
#include "lanlink/protocol/ipv4.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace lanlink;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Operation> void rejects(Operation operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid Ethernet frame accepted");
}
}
int main() {
    try {
        protocol::NetworkId id{}; id[0] = std::byte{1};
        const network::VirtualNetworkAddress a{id, 0x0a400001U, 0x0a400000U, 24};
        const network::VirtualNetworkAddress b{id, 0x0a400002U, 0x0a400000U, 24};
        std::vector<std::byte> frame(42);
        std::fill_n(frame.begin(), 6, std::byte{255});
        const auto mac = network::virtual_mac(a.address);
        std::copy(mac.begin(), mac.end(), frame.begin() + 6);
        frame[12] = std::byte{8}; frame[13] = std::byte{6};
        frame[15] = std::byte{1}; frame[16] = std::byte{8};
        frame[18] = std::byte{6}; frame[19] = std::byte{4}; frame[21] = std::byte{1};
        std::copy(mac.begin(), mac.end(), frame.begin() + 22);
        frame[28] = frame[38] = std::byte{10}; frame[29] = frame[39] = std::byte{64};
        frame[31] = std::byte{1}; frame[41] = std::byte{2};
        const auto packet = network::encapsulate_ethernet(frame, a);
        require(network::decapsulate_ethernet(packet, b) == frame, "ARP Ethernet header not preserved");
        auto corrupt = frame; corrupt[6] = std::byte{4};
        rejects([&] { static_cast<void>(network::encapsulate_ethernet(corrupt, a)); });
        corrupt = frame; corrupt[31] = std::byte{7};
        rejects([&] { static_cast<void>(network::encapsulate_ethernet(corrupt, a)); });
        corrupt = frame; corrupt[38] = std::byte{192};
        rejects([&] { static_cast<void>(network::encapsulate_ethernet(corrupt, a)); });
        for (const auto type : {0x8100U, 0x88ccU, 0x86ddU, 0x0026U}) {
            corrupt = frame; corrupt[12] = std::byte{static_cast<unsigned char>(type >> 8U)};
            corrupt[13] = std::byte{static_cast<unsigned char>(type)};
            rejects([&] { static_cast<void>(network::encapsulate_ethernet(corrupt, a)); });
        }
        for (std::size_t size = 0; size < packet.size(); ++size)
            rejects([&] { static_cast<void>(network::decapsulate_ethernet(std::span(packet).first(size), b)); });
        auto modified = packet; modified[31] = std::byte{2};
        rejects([&] { static_cast<void>(network::decapsulate_ethernet(modified, b)); });
        modified = packet; modified[12] = std::byte{192}; protocol::update_ipv4_checksum(modified);
        rejects([&] { static_cast<void>(network::decapsulate_ethernet(modified, b)); });
        frame.resize(network::ethernet_mtu + 15U);
        rejects([&] { static_cast<void>(network::encapsulate_ethernet(frame, a)); });
        std::cout << "Ethernet identity, scope, framing and size checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
