#include "lanlink/session/relay_packet_path.hpp"
#include "lanlink/session/relay_session.hpp"

#include <stdexcept>
#include <utility>

namespace lanlink::session {

RelayPacketPath::RelayPacketPath(RelaySession& session) noexcept : session_(session) {}
RelayPacketPath::~RelayPacketPath() { stop(); }
void RelayPacketPath::start() { session_.start(); }
void RelayPacketPath::stop() noexcept { session_.stop(); }

void RelayPacketPath::set_receive_handler(ReceiveHandler handler) {
    session_.client().set_encrypted_packet_handler(std::move(handler));
}

transport::PacketSendResult RelayPacketPath::send(const protocol::EncryptedNetworkPacket& packet) {
    using transport::PacketSendResult;
    if (!session_.client().authenticated()) return PacketSendResult::unavailable;
    try {
        session_.client().send_encrypted_packet(packet);
        return PacketSendResult::accepted;
    } catch (const std::invalid_argument&) {
        return PacketSendResult::invalid_packet;
    } catch (const std::exception&) {
        return session_.client().authenticated() ? PacketSendResult::backpressure
                                                 : PacketSendResult::unavailable;
    }
}

transport::PathStatus RelayPacketPath::status() const {
    const auto d = session_.client().diagnostics();
    return {d.tcp_fallback ? transport::PathKind::relay_tls : transport::PathKind::relay_quic,
            d.authenticated ? transport::PathLifecycle::authenticated :
                session_.finished() ? transport::PathLifecycle::stopped : transport::PathLifecycle::connecting,
            d.rtt_us, d.sent_bytes, d.received_bytes};
}

}
