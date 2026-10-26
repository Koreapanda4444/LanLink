#pragma once

#include "lanlink/transport/packet_path.hpp"

namespace lanlink::session {

class RelaySession;

class RelayPacketPath final : public transport::PacketPath {
public:
    explicit RelayPacketPath(RelaySession& session) noexcept;
    ~RelayPacketPath() override;
    void start() override;
    void stop() noexcept override;
    void set_receive_handler(ReceiveHandler handler) override;
    [[nodiscard]] transport::PacketSendResult send(const protocol::EncryptedNetworkPacket& packet) override;
    [[nodiscard]] transport::PathStatus status() const override;

private:
    RelaySession& session_;
};

}
