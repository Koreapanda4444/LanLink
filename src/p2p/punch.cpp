#include "lanlink/p2p/punch.hpp"
#include "lanlink/protocol/binary.hpp"
#include <algorithm>
namespace lanlink::p2p {
PeerBinding PeerBinding::reversed() const {
    auto b=*this;std::swap(b.local_device,b.peer_device);std::swap(b.local_session,b.peer_session);std::swap(b.local_generation,b.peer_generation);return b;
}
std::vector<std::byte> encode_binding(const PeerBinding& b) {
    protocol::BinaryWriter w;w.append(b.network_id);w.put(b.key_epoch);w.put(b.local_generation);w.put(b.peer_generation);
    w.append(b.local_device);w.append(b.peer_device);w.append(b.local_session);w.append(b.peer_session);return std::move(w).finish();
}
PeerBinding decode_binding(std::span<const std::byte> bytes) {
    protocol::BinaryReader r(bytes);PeerBinding b;b.network_id=r.array<16>();b.key_epoch=r.get<std::uint64_t>();
    b.local_generation=r.get<std::uint64_t>();b.peer_generation=r.get<std::uint64_t>();
    b.local_device=r.array<32>();b.peer_device=r.array<32>();b.local_session=r.array<32>();b.peer_session=r.array<32>();r.finish();return b;
}
PunchSession::PunchSession(const auth::DeviceIdentity& identity,auth::SignedDeviceKey peer,PeerBinding binding,
    std::vector<Candidate> remote,Clock::time_point now):identity_(identity),peer_key_(peer),binding_(binding),remote_(std::move(remote)),
    nonce_(auth::random_nonce()),deadline_(now+std::chrono::seconds{5}),next_(now) {
    if(identity.device_id()!=binding.local_device || !auth::verify_signed_device_key(peer,binding.peer_device) ||
        peer.encryption_public_key!=binding.peer_session || !binding.key_epoch || !binding.local_generation || !binding.peer_generation ||
        remote_.empty() || remote_.size()>max_candidates)throw std::invalid_argument("unauthorized punch session");
    for(const auto& c:remote_)if(!usable_endpoint(c.endpoint))throw std::invalid_argument("invalid punch endpoint");
}
std::vector<std::byte> PunchSession::message(bool reply,const auth::Nonce& echo) const {
    protocol::BinaryWriter w;w.put<std::uint32_t>(0x4c4c5031);w.put<std::uint8_t>(reply?2:1);w.append(encode_binding(binding_));w.append(nonce_);w.append(echo);
    auto bytes=std::move(w).finish();const auto signature=identity_.sign(bytes);bytes.insert(bytes.end(),signature.begin(),signature.end());return bytes;
}
std::vector<ProbeDatagram> PunchSession::tick(Clock::time_point now) {
    std::vector<ProbeDatagram> result;if(finished(now) || now<next_ || sent_>=32)return result;
    const auto bytes=message(false,{});
    for(const auto& c:remote_) {if(sent_>=32)break;result.push_back({c.endpoint,bytes});++sent_;}
    next_=now+std::chrono::milliseconds{50*(1U<<std::min(round_++,4U))};return result;
}
std::optional<ProbeDatagram> PunchSession::receive(const Datagram& d,Clock::time_point now) {
    if(now>=deadline_ || d.bytes.size()!=5+168+64+64 || responses_>=32)return std::nullopt;
    if(std::none_of(remote_.begin(),remote_.end(),[&](auto c){return c.endpoint==d.source;}))return std::nullopt;
    try {
        protocol::BinaryReader r(d.bytes);if(r.get<std::uint32_t>()!=0x4c4c5031)return std::nullopt;
        const auto kind=r.get<std::uint8_t>();if(decode_binding(r.bytes(168))!=binding_.reversed())return std::nullopt;
        const auto nonce=r.array<32>(),echo=r.array<32>();const auto signature=r.array<64>();r.finish();
        if((kind!=1 && kind!=2) || !auth::verify_signature(peer_key_.identity_public_key,std::span(d.bytes).first(d.bytes.size()-64),signature))return std::nullopt;
        if(kind==2) {if(echo==nonce_){selected_=d.source;if(std::find(verified_.begin(),verified_.end(),d.source)==verified_.end())verified_.push_back(d.source);}return std::nullopt;}
        ++responses_;return ProbeDatagram{d.source,message(true,nonce)};
    }catch(const std::exception&){return std::nullopt;}
}
bool PunchSession::finished(Clock::time_point now) const noexcept {return selected_.has_value() || now>=deadline_;}
std::optional<Endpoint> run_punch(UdpSocket& socket,PunchSession& session) {
    while(!session.finished()) {
        for(const auto& d:session.tick())static_cast<void>(socket.send(d.destination,d.bytes));
        if(const auto d=socket.receive(std::chrono::milliseconds{20})) if(const auto reply=session.receive(*d))static_cast<void>(socket.send(reply->destination,reply->bytes));
    }
    return session.selected();
}
}
