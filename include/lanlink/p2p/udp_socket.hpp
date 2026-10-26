#pragma once
#include "lanlink/p2p/candidates.hpp"
#include <chrono>
#include <memory>
#include <optional>
namespace lanlink::p2p {
struct Datagram {Endpoint source; std::vector<std::byte> bytes;};
class UdpSocket {
public:
    explicit UdpSocket(Endpoint local = {});
    ~UdpSocket();
    UdpSocket(const UdpSocket&)=delete;
    UdpSocket& operator=(const UdpSocket&)=delete;
    [[nodiscard]] Endpoint local_endpoint() const;
    [[nodiscard]] bool send(Endpoint destination,std::span<const std::byte> bytes) const;
    [[nodiscard]] std::optional<Datagram> receive(std::chrono::milliseconds timeout) const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
}
