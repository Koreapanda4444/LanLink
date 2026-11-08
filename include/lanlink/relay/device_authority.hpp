#pragma once
#include "lanlink/storage/relay_store.hpp"
#include "lanlink/protocol/device_registration.hpp"
namespace lanlink::relay {
class DeviceAuthority {
public:
    DeviceAuthority(storage::RelayStore& store,const auth::AuthToken& master,bool require_registration)
        :store_(store),master_(master),required_(require_registration){}
    std::string issue_code(std::int64_t now_ms,std::uint32_t lifetime_seconds=300);
    void revoke_code(std::string_view code);
    std::vector<std::byte> register_device(std::span<const std::byte> request,std::int64_t now_ms);
    bool verify(const auth::PublicKey& key,std::span<const std::byte> transcript,const auth::TokenProof& proof) const;
    bool allowed(const auth::DeviceId& device) const;
private:
    std::string credential(const auth::DeviceId& device) const;
    storage::RelayStore& store_;
    const auth::AuthToken& master_;
    bool required_;
};
}
