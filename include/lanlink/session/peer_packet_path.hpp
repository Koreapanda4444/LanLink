#pragma once
#include "lanlink/p2p/local_candidates.hpp"
#include "lanlink/session/relay_session.hpp"
#include "lanlink/transport/packet_path.hpp"
namespace lanlink::session {
struct PeerPathOptions {
    bool enabled=true,allow_loopback=false;
    std::vector<p2p::Endpoint> stun_servers;
    std::function<std::vector<p2p::InterfaceAddress>()> interfaces=p2p::interface_addresses;
};
class PeerPacketPath final : public transport::PacketPath {
public:
    PeerPacketPath(RelaySession& session,const std::filesystem::path& identity_file,core::Logger& logger,PeerPathOptions options={});
    ~PeerPacketPath() override;
    void set_direct_enabled(bool enabled) noexcept;
    void revalidate() noexcept;
    void start() override;
    void stop() noexcept override;
    void set_receive_handler(ReceiveHandler handler) override;
    [[nodiscard]] transport::PacketSendResult send(const protocol::EncryptedNetworkPacket& packet) override;
    [[nodiscard]] transport::PathStatus status() const override;
    [[nodiscard]] std::vector<protocol::PeerPathStatus> peer_status() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
