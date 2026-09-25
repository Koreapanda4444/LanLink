#include "lanlink/relay/network_service.hpp"
#include "lanlink/storage/relay_store.hpp"

#include <sqlite3.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace storage = lanlink::storage;
namespace protocol = lanlink::protocol;
namespace relay = lanlink::relay;

void expect(const bool condition, const std::string_view description) {
    if (!condition) {
        throw std::runtime_error(std::string(description));
    }
}

template <typename Function>
void expect_error(Function&& function, const std::string_view description) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(std::string(description));
}

storage::NetworkId network_id(const std::uint32_t seed) {
    storage::NetworkId result{};
    result[0] = std::byte{0xa5};
    result[1] = static_cast<std::byte>((seed >> 24U) & 0xffU);
    result[2] = static_cast<std::byte>((seed >> 16U) & 0xffU);
    result[3] = static_cast<std::byte>((seed >> 8U) & 0xffU);
    result[4] = static_cast<std::byte>(seed & 0xffU);
    return result;
}

lanlink::auth::DeviceId device_id(const std::uint32_t seed) {
    lanlink::auth::DeviceId result{};
    result[0] = std::byte{0xb5};
    result[1] = static_cast<std::byte>((seed >> 24U) & 0xffU);
    result[2] = static_cast<std::byte>((seed >> 16U) & 0xffU);
    result[3] = static_cast<std::byte>((seed >> 8U) & 0xffU);
    result[4] = static_cast<std::byte>(seed & 0xffU);
    return result;
}

template <std::size_t Size>
std::string blob_sql(const std::array<std::byte, Size>& value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "X'";
    for (const auto byte : value) {
        const auto number = std::to_integer<unsigned int>(byte);
        result.push_back(digits[number >> 4U]);
        result.push_back(digits[number & 15U]);
    }
    result += "'";
    return result;
}

void expect_constraint(sqlite3* database,
                       const std::string& sql,
                       const std::string_view description) {
    char* detail = nullptr;
    const auto status = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &detail);
    sqlite3_free(detail);
    expect(status != SQLITE_OK, description);
}

void execute_sql(sqlite3* database, const char* sql) {
    char* detail = nullptr;
    const auto status = sqlite3_exec(database, sql, nullptr, nullptr, &detail);
    const std::string message = detail ? detail : "SQLite test command failed";
    sqlite3_free(detail);
    if (status != SQLITE_OK) {
        throw std::runtime_error(message);
    }
}

std::int64_t query_sql_integer(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        throw std::runtime_error("SQLite test query returned no row");
    }
    const auto result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return result;
}

void test_v2_migration(const std::filesystem::path& directory) {
    const auto path = directory / "previous-relay.db";
    const auto a = network_id(201);
    const auto b = network_id(202);
    const auto owner = device_id(201);
    const auto member = device_id(202);
    const auto invited = device_id(203);
    const auto requester = device_id(204);

    {
        storage::RelayStore previous(path);
        for (const auto& device : {owner, member, invited, requester}) {
            previous.record_device(device, 1);
        }
        previous.create_network({a, "previous a", owner, 1});
        previous.create_network({b, "previous b", owner, 2});
        expect(previous.add_member(a, member, 3) &&
                   previous.add_invitation(a, invited, 4) &&
                   previous.add_join_request(a, requester, 5),
               "previous-version fixture includes members and pending access");
    }

    sqlite3* database = nullptr;
    expect(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK,
           "open previous-version database fixture");
    execute_sql(database,
                "PRAGMA foreign_keys = ON;"
                "DROP TABLE virtual_ipv4_leases;"
                "DROP TABLE network_subnets;"
                "PRAGMA user_version = 2;");
    sqlite3_close(database);

    {
        storage::RelayStore upgraded(path);
        expect(upgraded.schema_version() == storage::current_schema_version,
               "v2 database migrates to v3");
        const auto state = upgraded.load_state();
        expect(state.networks.size() == 2 && state.memberships.size() == 3 &&
                   state.invitations.size() == 1 && state.join_requests.size() == 1 &&
                   state.subnets.size() == 2 && state.leases.size() == 3,
               "migration preserves previous network, membership, and pending access rows");
        expect(upgraded.find_subnet(a)->network_address ==
                   storage::virtual_ipv4_pool_first &&
                   upgraded.find_subnet(b)->network_address ==
                       storage::virtual_ipv4_pool_first +
                           storage::virtual_ipv4_subnet_size &&
                   upgraded.find_virtual_ipv4_lease(a, owner)->address ==
                       storage::virtual_ipv4_pool_first + 1 &&
                   upgraded.find_virtual_ipv4_lease(a, member)->address ==
                       storage::virtual_ipv4_pool_first + 2 &&
                   upgraded.find_virtual_ipv4_lease(b, owner)->address ==
                       storage::virtual_ipv4_pool_first +
                           storage::virtual_ipv4_subnet_size + 1,
               "existing networks and members receive deterministic distinct addresses");
        expect(!upgraded.find_virtual_ipv4_lease(a, invited) &&
                   !upgraded.find_virtual_ipv4_lease(a, requester),
               "nonmembers receive no virtual IPv4 lease during migration");
    }

    storage::RelayStore restarted(path);
    expect(restarted.find_virtual_ipv4_lease(a, member)->address ==
               storage::virtual_ipv4_pool_first + 2 &&
               restarted.find_invitation(a, invited) &&
               restarted.find_join_request(a, requester),
           "migrated addresses and pending access survive restart");
}

void test_migration_capacity_rollback(const std::filesystem::path& directory) {
    const auto path = directory / "oversized-previous-relay.db";
    const auto network = network_id(301);
    const auto owner = device_id(301);
    const auto extra = device_id(9999);
    {
        storage::RelayStore previous(path);
        previous.record_device(owner, 1);
        previous.create_network({network, "oversized", owner, 1});
        for (std::uint32_t index = 0; index < 253; ++index) {
            const auto device = device_id(2000 + index);
            previous.record_device(device, 2);
            expect(previous.add_member(network, device, 2),
                   "previous-version fixture fills the address pool");
        }
        previous.record_device(extra, 3);
    }

    sqlite3* database = nullptr;
    expect(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK,
           "open oversized previous-version database");
    execute_sql(database,
                "PRAGMA foreign_keys = ON;"
                "DROP TABLE virtual_ipv4_leases;"
                "DROP TABLE network_subnets;"
                "PRAGMA user_version = 2;");
    const auto insert = "INSERT INTO memberships VALUES(" + blob_sql(network) + ", " +
                        blob_sql(extra) + ", 0, 3);";
    execute_sql(database, insert.c_str());
    sqlite3_close(database);

    expect_error([&] { storage::RelayStore migrated(path); },
                 "oversized existing network fails migration without dropping members");
    expect(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK,
           "inspect failed migration database");
    expect(query_sql_integer(database, "PRAGMA user_version;") == 2 &&
               query_sql_integer(database,
                                 "SELECT COUNT(*) FROM sqlite_master "
                                 "WHERE type = 'table' AND name IN "
                                 "('network_subnets', 'virtual_ipv4_leases');") == 0 &&
               query_sql_integer(database,
                                 "SELECT COUNT(*) FROM memberships;") == 255,
           "failed migration rolls back schema and data atomically");
    sqlite3_close(database);
}

void test_allocation_and_constraints(const std::filesystem::path& directory) {
    const auto database_path = directory / "subnets.db";
    const auto a = network_id(1);
    const auto b = network_id(2);
    const auto c = network_id(3);
    const auto owner = device_id(1);
    const auto member = device_id(2);
    const auto replacement = device_id(3);

    {
        storage::RelayStore store(database_path);
        store.record_device(owner, 1);
        store.record_device(member, 1);
        store.record_device(replacement, 1);
        store.create_network({a, "network a", owner, 1});
        store.create_network({b, "network b", owner, 2});
        const auto subnet_a = store.find_subnet(a);
        const auto subnet_b = store.find_subnet(b);
        expect(subnet_a && subnet_a->network_address == storage::virtual_ipv4_pool_first &&
                   subnet_a->prefix_length == 24 &&
                   subnet_b && subnet_b->network_address ==
                                   storage::virtual_ipv4_pool_first +
                                       storage::virtual_ipv4_subnet_size,
               "networks receive distinct /24 subnets");
        expect(storage::format_virtual_ipv4(subnet_b->network_address) == "10.64.1.0",
               "address formatting uses IPv4 network byte order");
        expect(store.find_virtual_ipv4_lease(a, owner)->address ==
                   storage::virtual_ipv4_pool_first + 1 &&
                   store.find_virtual_ipv4_lease(b, owner)->address ==
                       subnet_b->network_address + 1,
               "owner receives first usable host in each subnet");
        expect(store.add_member(a, member, 3) && store.add_member(b, member, 3),
               "device can join multiple networks");
        expect(store.find_virtual_ipv4_lease(a, member)->address ==
                   subnet_a->network_address + 2 &&
                   store.find_virtual_ipv4_lease(b, member)->address ==
                       subnet_b->network_address + 2,
               "leases are unique across and within subnets");
        expect(!store.add_member(a, member, 4), "duplicate membership leaves lease unchanged");
        store.record_device(member, 5);
        expect(store.find_virtual_ipv4_lease(a, member)->address ==
                   subnet_a->network_address + 2,
               "reconnecting a device preserves its virtual IPv4 address");
        expect(store.remove_member(a, member) &&
                   !store.find_virtual_ipv4_lease(a, member),
               "leaving releases only the departing member lease");
        expect(store.add_member(a, replacement, 6) &&
                   store.find_virtual_ipv4_lease(a, replacement)->address ==
                       subnet_a->network_address + 2,
               "new member reuses the lowest free host without moving owner");
    }

    {
        storage::RelayStore reopened(database_path);
        expect(reopened.find_subnet(b)->network_address ==
                   storage::virtual_ipv4_pool_first + storage::virtual_ipv4_subnet_size &&
                   reopened.find_virtual_ipv4_lease(b, member)->address ==
                       storage::virtual_ipv4_pool_first +
                           storage::virtual_ipv4_subnet_size + 2,
               "network subnet and member lease survive database restart");

        sqlite3* database = nullptr;
        expect(sqlite3_open(database_path.string().c_str(), &database) == SQLITE_OK,
               "open direct SQLite collision test connection");
        sqlite3_exec(database, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
        const auto member_b = " WHERE network_id = " + blob_sql(b) +
                              " AND device_id = " + blob_sql(member) + ";";
        expect_constraint(database,
                          "UPDATE virtual_ipv4_leases SET ipv4_address = " +
                              std::to_string(storage::virtual_ipv4_pool_first + 257) +
                              member_b,
                          "SQLite rejects duplicate virtual IPv4 addresses");
        expect_constraint(database,
                          "UPDATE virtual_ipv4_leases SET ipv4_address = " +
                              std::to_string(storage::virtual_ipv4_pool_first + 511) +
                              member_b,
                          "SQLite rejects network broadcast address");
        expect_constraint(database,
                          "UPDATE virtual_ipv4_leases SET ipv4_address = " +
                              std::to_string(storage::virtual_ipv4_pool_first + 3) +
                              member_b,
                          "SQLite rejects virtual IP from another network");
        expect_constraint(database,
                          "INSERT INTO virtual_ipv4_leases VALUES(" + blob_sql(b) +
                              ", " + blob_sql(replacement) + ", " +
                              std::to_string(storage::virtual_ipv4_pool_first + 259) + ");",
                          "SQLite refuses a lease for a non-member");
        expect_constraint(database,
                          "UPDATE network_subnets SET subnet_address = " +
                              std::to_string(storage::virtual_ipv4_pool_first + 512) +
                              " WHERE network_id = " + blob_sql(b) + ";",
                          "SQLite prevents moving a subnet with active leases");
        sqlite3_close(database);

        expect(reopened.delete_network(a), "first network removed");
        expect(!reopened.find_subnet(a) && reopened.list_virtual_ipv4_leases(a).empty(),
               "deletion cascades subnet and leases");
        reopened.create_network({c, "network c", owner, 7});
        expect(reopened.find_subnet(c)->network_address ==
                   storage::virtual_ipv4_pool_first &&
                   reopened.find_subnet(b)->network_address ==
                       storage::virtual_ipv4_pool_first + storage::virtual_ipv4_subnet_size,
               "freed subnet reused without changing existing network");
    }
}

void test_capacity_and_atomicity(const std::filesystem::path& directory) {
    const auto database_path = directory / "capacity.db";
    const auto network = network_id(100);
    const auto owner = device_id(100);
    storage::RelayStore store(database_path);
    store.record_device(owner, 1);
    store.create_network({network, "full network", owner, 1});
    for (std::uint32_t index = 0; index < 253; ++index) {
        const auto device = device_id(1000 + index);
        store.record_device(device, index + 2);
        expect(store.add_member(network, device, index + 2),
               "available host must be assigned to new member");
        expect(store.find_virtual_ipv4_lease(network, device)->address ==
                   storage::virtual_ipv4_pool_first + index + 2,
               "host allocation is deterministic and contiguous");
    }
    expect(store.list_virtual_ipv4_leases(network).size() == 254,
           "all 254 usable hosts allocated exactly once");
    const auto extra = device_id(9999);
    store.record_device(extra, 500);
    expect_error([&] { static_cast<void>(store.add_member(network, extra, 500)); },
                 "full subnet rejects a new member");
    expect(!store.find_virtual_ipv4_lease(network, extra) &&
               store.list_members(network).size() == 254,
           "failed lease allocation rolls back membership atomically");
    relay::NetworkService service(store);
    expect(service.invite_member(owner, {network, extra}, 500).result.code ==
               protocol::NetworkResultCode::success &&
               service.join_network(extra, {network}, 501).result.code ==
                   protocol::NetworkResultCode::limit_reached &&
               store.find_invitation(network, extra).has_value(),
           "full subnet reports limit reached and preserves pending invitation");
    expect(store.remove_member(network, device_id(1000)),
           "member can leave a full subnet");
    expect(service.join_network(extra, {network}, 502).result.code ==
               protocol::NetworkResultCode::success &&
               store.find_virtual_ipv4_lease(network, extra)->address ==
                   storage::virtual_ipv4_pool_first + 2,
           "freed host can be allocated without shifting remaining leases");
    const auto requester = device_id(10000);
    expect(service.join_network(requester, {network}, 503).result.code ==
               protocol::NetworkResultCode::pending_approval &&
               service.approve_member(owner, {network, requester}, 504).result.code ==
                   protocol::NetworkResultCode::limit_reached &&
               !store.find_virtual_ipv4_lease(network, requester) &&
               store.find_join_request(network, requester).has_value(),
           "approval of a full subnet reports limit reached without losing request");
    expect(store.remove_member(network, device_id(1001)) &&
               service.approve_member(owner, {network, requester}, 505).result.code ==
                   protocol::NetworkResultCode::success &&
               store.find_virtual_ipv4_lease(network, requester)->address ==
                   storage::virtual_ipv4_pool_first + 3,
           "approval succeeds after capacity becomes available");
    storage::RelayStore reopened(database_path);
    expect(reopened.find_virtual_ipv4_lease(network, extra)->address ==
               storage::virtual_ipv4_pool_first + 2 &&
               reopened.find_virtual_ipv4_lease(network, requester)->address ==
                   storage::virtual_ipv4_pool_first + 3 &&
               reopened.list_virtual_ipv4_leases(network).size() == 254,
           "capacity and lease restoration persist after restart");
}

}

int main() {
    try {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() /
                               ("lanlink-virtual-ipv4-tests-" + std::to_string(suffix));
        std::filesystem::create_directories(directory);
        test_allocation_and_constraints(directory);
        test_capacity_and_atomicity(directory);
        test_v2_migration(directory);
        test_migration_capacity_rollback(directory);
        std::cout << "virtual IPv4 lease tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "virtual IPv4 lease tests failed: " << error.what() << '\n';
        return 1;
    }
}
