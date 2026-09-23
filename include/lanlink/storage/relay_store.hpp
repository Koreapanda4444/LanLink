#pragma once

#include "lanlink/auth/identity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lanlink::storage {

inline constexpr std::size_t network_id_size = 16;
inline constexpr int current_schema_version = 1;

using NetworkId = std::array<std::byte, network_id_size>;

enum class MembershipRole : std::int32_t {
    member = 0,
    owner = 1,
};

struct DeviceRecord {
    auth::DeviceId id{};
    std::int64_t first_seen_at_ms = 0;
    std::int64_t last_seen_at_ms = 0;

    bool operator==(const DeviceRecord&) const = default;
};

struct NetworkRecord {
    NetworkId id{};
    std::string name;
    auth::DeviceId owner_device_id{};
    std::int64_t created_at_ms = 0;

    bool operator==(const NetworkRecord&) const = default;
};

struct MembershipRecord {
    NetworkId network_id{};
    auth::DeviceId device_id{};
    MembershipRole role = MembershipRole::member;
    std::int64_t joined_at_ms = 0;

    bool operator==(const MembershipRecord&) const = default;
};

struct RelayState {
    std::vector<DeviceRecord> devices;
    std::vector<NetworkRecord> networks;
    std::vector<MembershipRecord> memberships;

    bool operator==(const RelayState&) const = default;
};

class RelayStore {
public:
    explicit RelayStore(std::filesystem::path database_path);
    ~RelayStore();

    RelayStore(const RelayStore&) = delete;
    RelayStore& operator=(const RelayStore&) = delete;
    RelayStore(RelayStore&&) = delete;
    RelayStore& operator=(RelayStore&&) = delete;

    [[nodiscard]] const std::filesystem::path& database_path() const noexcept;
    [[nodiscard]] int schema_version() const;
    [[nodiscard]] std::string journal_mode() const;

    void record_device(const auth::DeviceId& device_id, std::int64_t seen_at_ms);
    [[nodiscard]] std::optional<DeviceRecord> find_device(
        const auth::DeviceId& device_id) const;

    void create_network(const NetworkRecord& network);
    [[nodiscard]] std::optional<NetworkRecord> find_network(
        const NetworkId& network_id) const;
    [[nodiscard]] bool delete_network(const NetworkId& network_id);

    [[nodiscard]] bool add_member(const NetworkId& network_id,
                                  const auth::DeviceId& device_id,
                                  std::int64_t joined_at_ms);
    [[nodiscard]] bool remove_member(const NetworkId& network_id,
                                     const auth::DeviceId& device_id);
    [[nodiscard]] std::vector<MembershipRecord> list_members(
        const NetworkId& network_id) const;
    [[nodiscard]] std::vector<NetworkRecord> list_networks_for_device(
        const auth::DeviceId& device_id) const;

    [[nodiscard]] RelayState load_state() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
