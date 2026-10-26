#pragma once
#include "lanlink/p2p/punch.hpp"
#include "lanlink/transport/packet_path.hpp"
namespace lanlink::p2p {
struct DirectQuicOptions {
    PeerBinding binding;
    auth::SignedDeviceKey peer_key;
    std::uint32_t local_ipv4=0,peer_ipv4=0;
    std::uint16_t local_port=0;
};
class DirectQuic {
public:
    DirectQuic(const auth::DeviceIdentity& identity,DirectQuicOptions options,transport::PacketPath::ReceiveHandler receive);
    ~DirectQuic();
    DirectQuic(const DirectQuic&)=delete;DirectQuic& operator=(const DirectQuic&)=delete;
    void start();
    void connect(Endpoint endpoint);
    void stop() noexcept;
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] bool authenticated() const;
    [[nodiscard]] transport::PacketSendResult send(const protocol::EncryptedNetworkPacket& packet);
    [[nodiscard]] transport::PathStatus status() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
