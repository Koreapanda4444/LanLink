#pragma once
#include "lanlink/protocol/server_profiles.hpp"
#include <filesystem>
#include <map>
namespace lanlink::client {
struct StoredProfile { protocol::ServerProfile profile; std::string identity, credential; };
class ProfileStore {
public:
    explicit ProfileStore(std::filesystem::path directory);
    protocol::ServerState state() const;
    std::string save(protocol::ServerProfile profile, std::string_view credential = {});
    void erase(std::string_view id);
    void select(std::string_view id);
    StoredProfile get(std::string_view id) const;
    std::filesystem::path identity_path(std::string_view id) const;
    std::filesystem::path credential_path(std::string_view id) const;
    void save_device_credential(std::string_view id, std::string_view secret);
private:
    void persist(const std::map<std::string,StoredProfile>& profiles,std::string_view selected,std::uint64_t revision,std::uint64_t epoch);
    std::filesystem::path directory_;
    std::map<std::string,StoredProfile> profiles_;
    std::string selected_;
    std::uint64_t revision_ = 1, epoch_ = 1;
};
}
