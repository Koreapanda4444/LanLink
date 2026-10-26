#pragma once
#include "lanlink/p2p/udp_socket.hpp"
#include "lanlink/auth/device_keys.hpp"
#include <optional>
namespace lanlink::p2p {
struct PeerBinding {
    protocol::NetworkId network_id{};
    std::uint64_t key_epoch=0,local_generation=0,peer_generation=0;
    auth::DeviceId local_device{},peer_device{};
    auth::EncryptionPublicKey local_session{},peer_session{};
    [[nodiscard]] PeerBinding reversed() const;
    bool operator==(const PeerBinding&) const=default;
};
[[nodiscard]] std::vector<std::byte> encode_binding(const PeerBinding& binding);
[[nodiscard]] PeerBinding decode_binding(std::span<const std::byte> bytes);
struct ProbeDatagram {Endpoint destination;std::vector<std::byte> bytes;};
class PunchSession {
public:
    using Clock=std::chrono::steady_clock;
    PunchSession(const auth::DeviceIdentity& identity,auth::SignedDeviceKey peer_key,PeerBinding binding,
        std::vector<Candidate> remote,Clock::time_point now=Clock::now());
    [[nodiscard]] std::vector<ProbeDatagram> tick(Clock::time_point now=Clock::now());
    [[nodiscard]] std::optional<ProbeDatagram> receive(const Datagram& datagram,Clock::time_point now=Clock::now());
    [[nodiscard]] std::optional<Endpoint> selected() const noexcept{return selected_;}
    [[nodiscard]] bool finished(Clock::time_point now=Clock::now()) const noexcept;
    [[nodiscard]] unsigned sent_probes() const noexcept{return sent_;}
private:
    std::vector<std::byte> message(bool reply,const auth::Nonce& echo) const;
    const auth::DeviceIdentity& identity_;auth::SignedDeviceKey peer_key_;PeerBinding binding_;
    std::vector<Candidate> remote_;auth::Nonce nonce_;
    Clock::time_point deadline_,next_;unsigned round_=0,sent_=0,responses_=0;
    std::optional<Endpoint> selected_;
};
[[nodiscard]] std::optional<Endpoint> run_punch(UdpSocket& socket,PunchSession& session);
}
