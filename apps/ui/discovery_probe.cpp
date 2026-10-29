#include "lanlink/p2p/udp_socket.hpp"
#include "lanlink/protocol/game_profiles.hpp"

#include <openssl/rand.h>
#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
unsigned number(std::string_view value, unsigned maximum) {
    unsigned result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result > maximum)
        throw std::invalid_argument("invalid numeric address or port");
    return result;
}
std::uint32_t ipv4(std::string_view value) {
    std::uint32_t result = 0;
    for (unsigned index = 0; index < 4; ++index) {
        const auto dot = value.find('.');
        if ((index < 3) != (dot != std::string_view::npos)) throw std::invalid_argument("use a numeric IPv4 address");
        result = (result << 8U) | number(value.substr(0, dot), 255);
        if (dot != std::string_view::npos) value.remove_prefix(dot + 1);
    }
    return result;
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 3 && argc != 4) throw std::invalid_argument("usage: lanlink-discovery-probe HOST_IPV4 PORT [LOCAL_IPV4] (Teeworlds 0.7.5)");
        const lanlink::p2p::Endpoint target{ipv4(argv[1]), static_cast<std::uint16_t>(number(argv[2], 65535))};
        lanlink::p2p::UdpSocket socket({argc == 4 ? ipv4(argv[3]) : 0, 0});
        std::uint32_t token = 0;
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&token), sizeof(token)) != 1) throw std::runtime_error("query randomness unavailable");
        token &= 0x7fffffffU;
        const auto challenge = static_cast<std::uint8_t>(token & 63U);
        const auto exchange = [&](const auto& request, auto decode) {
            if (!socket.send(target, request)) throw std::runtime_error("cannot send game query");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (std::chrono::steady_clock::now() < deadline) {
                const auto reply = socket.receive(std::chrono::milliseconds{100});
                if (reply && reply->source == target) if (auto value = decode(reply->bytes)) return value;
            }
            throw std::runtime_error("no valid Teeworlds 0.7.5 response; check host port, virtual interface and firewall");
        };
        const auto server = exchange(lanlink::protocol::teeworlds_token_request(token),
            [&](const auto bytes) { return lanlink::protocol::teeworlds_server_token(bytes, token); });
        const auto info = exchange(lanlink::protocol::teeworlds_info_request(token, *server, challenge),
            [&](const auto bytes) { return lanlink::protocol::teeworlds_server_info(bytes, token, challenge); });
        std::cout << "Teeworlds " << info->version << " server query verified: " << info->name << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
