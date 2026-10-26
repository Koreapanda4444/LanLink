#include "lanlink/p2p/candidates.hpp"
#include "lanlink/protocol/binary.hpp"
#include <algorithm>
#include <set>

namespace lanlink::p2p {
bool usable_endpoint(Endpoint e, bool loopback) noexcept {
    const auto first = e.ipv4 >> 24;
    return e.port != 0 && first != 0 && first < 224 &&
        (loopback || first != 127) && (e.ipv4 & 0xffff0000U) != 0xa9fe0000U &&
        e.ipv4 != 0xffffffffU;
}
namespace {
void validate(const CandidateExchange& v) {
    auto nonzero=[](const auto& a){return std::any_of(a.begin(),a.end(),[](auto b){return b!=std::byte{0};});};
    if (!nonzero(v.network_id) || !nonzero(v.sender) || !nonzero(v.recipient) || v.sender == v.recipient ||
        !nonzero(v.sender_session) || !nonzero(v.recipient_session) || !v.key_epoch || !v.generation ||
        v.lifetime_ms < 1000 || v.lifetime_ms > 30'000 || v.candidates.empty() || v.candidates.size() > max_candidates)
        throw std::invalid_argument("invalid peer candidate exchange");
    std::set<Endpoint> seen;
    for (const auto& c : v.candidates) if (!usable_endpoint(c.endpoint) || !c.priority ||
        (c.kind != CandidateKind::host && c.kind != CandidateKind::server_reflexive) || !seen.insert(c.endpoint).second)
        throw std::invalid_argument("invalid peer candidate");
}
}
std::vector<std::byte> encode_candidates(const CandidateExchange& v) {
    validate(v); protocol::BinaryWriter w;
    w.put<std::uint8_t>(1); w.append(v.network_id); w.append(v.sender); w.append(v.recipient);
    w.append(v.sender_session); w.append(v.recipient_session);
    w.put(v.key_epoch); w.put(v.generation); w.put(v.lifetime_ms);
    w.put<std::uint8_t>(static_cast<std::uint8_t>(v.candidates.size()));
    for (auto c : v.candidates) {w.put(c.endpoint.ipv4); w.put(c.endpoint.port); w.put<std::uint8_t>(static_cast<std::uint8_t>(c.kind)); w.put(c.priority);}
    return std::move(w).finish();
}
CandidateExchange decode_candidates(std::span<const std::byte> b) {
    protocol::BinaryReader r(b); if (r.get<std::uint8_t>() != 1) throw std::invalid_argument("unknown candidate schema");
    CandidateExchange v; v.network_id=r.array<16>(); v.sender=r.array<32>(); v.recipient=r.array<32>();
    v.sender_session=r.array<32>(); v.recipient_session=r.array<32>();
    v.key_epoch=r.get<std::uint64_t>(); v.generation=r.get<std::uint64_t>(); v.lifetime_ms=r.get<std::uint32_t>();
    const auto count=r.get<std::uint8_t>(); if (count>max_candidates) throw std::invalid_argument("too many candidates");
    for (unsigned i=0;i<count;++i) {Candidate c; c.endpoint={r.get<std::uint32_t>(),r.get<std::uint16_t>()}; c.kind=static_cast<CandidateKind>(r.get<std::uint8_t>()); c.priority=r.get<std::uint32_t>(); v.candidates.push_back(c);}
    r.finish(); validate(v); return v;
}
}
