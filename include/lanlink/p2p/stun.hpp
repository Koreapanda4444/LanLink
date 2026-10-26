#pragma once
#include "lanlink/p2p/udp_socket.hpp"
namespace lanlink::p2p {
using StunTransaction=std::array<std::byte,12>;
[[nodiscard]] StunTransaction new_stun_transaction();
[[nodiscard]] std::vector<std::byte> stun_binding_request(const StunTransaction& transaction);
[[nodiscard]] Endpoint parse_stun_binding_response(std::span<const std::byte> bytes,const StunTransaction& expected);
[[nodiscard]] std::optional<Candidate> discover_stun_endpoint(UdpSocket& socket,Endpoint server,
    std::chrono::milliseconds timeout=std::chrono::milliseconds{1000});
}
