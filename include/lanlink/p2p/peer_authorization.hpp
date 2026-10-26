#pragma once
#include "lanlink/p2p/punch.hpp"
#include "lanlink/session/network_state.hpp"
namespace lanlink::p2p {
[[nodiscard]] bool peer_binding_authorized(const PeerBinding& binding,const auth::SignedDeviceKey& peer_key,
    std::uint32_t local_ipv4,std::uint32_t peer_ipv4,const session::NetworkSnapshot& snapshot);
[[nodiscard]] auth::Signature sign_peer_binding(const auth::DeviceIdentity& identity,const PeerBinding& binding,
    const auth::Nonce& sender_nonce,const auth::Nonce& receiver_nonce);
[[nodiscard]] bool verify_peer_binding(const auth::SignedDeviceKey& peer_key,const PeerBinding& expected,
    const auth::Nonce& sender_nonce,const auth::Nonce& receiver_nonce,const auth::Signature& signature);
}
