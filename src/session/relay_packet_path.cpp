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
    transport::PathStatus result;
    result.kind = !d.authenticated ? transport::PathKind::none : d.tcp_fallback
        ? transport::PathKind::relay_tls : transport::PathKind::relay_quic;
    result.lifecycle = d.authenticated ? transport::PathLifecycle::authenticated :
        session_.finished() ? transport::PathLifecycle::stopped : transport::PathLifecycle::connecting;
    result.rtt_us = d.rtt_us; result.sent_bytes = d.sent_bytes; result.received_bytes = d.received_bytes;
    result.reason = !d.authenticated ? transport::PathReason::awaiting_relay : d.tcp_fallback
        ? transport::PathReason::udp_blocked : transport::PathReason::relayed;
    result.error = d.error; result.sent_packets = d.sent_packets; result.received_packets = d.received_packets;
    result.dropped_packets = d.dropped_packets; result.lost_packets = d.lost_packets;
    result.retransmittable_packets = d.retransmittable_packets; result.reconnects = d.reconnects;
    return result;
}

}
