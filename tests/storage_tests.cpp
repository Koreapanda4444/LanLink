#include "lanlink/storage/relay_store.hpp"

#include <sqlite3.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view name) {
    if (!condition) {
        std::cerr << "failed: " << name << '\n';
        ++failures;
    }
}

template <typename Function>
void expect_error(Function&& function, const std::string_view name) {
    try {
        function();
        expect(false, name);
    } catch (const std::exception&) {
    }
}

std::filesystem::path make_test_directory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("lanlink-storage-tests-" + std::to_string(suffix));
    std::filesystem::create_directories(path);
    return path;
}

lanlink::auth::DeviceId device_id(const unsigned int seed) {
    lanlink::auth::DeviceId result{};

    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::byte>((seed + index * 17U) & 0xffU);
    }

    return result;
}

lanlink::storage::NetworkId network_id(const unsigned int seed) {
    lanlink::storage::NetworkId result{};

    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::byte>((seed + index * 29U) & 0xffU);
    }

    return result;
}

void execute_raw(sqlite3* database, const char* sql) {
    char* detail = nullptr;
    const int status = sqlite3_exec(database, sql, nullptr, nullptr, &detail);

    if (status == SQLITE_OK) {
        return;
    }

    const std::string message = detail == nullptr ? "SQLite test statement failed" : detail;
    sqlite3_free(detail);
    throw std::runtime_error(message);
}

std::int64_t query_raw_integer(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;

    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }

    const int status = sqlite3_step(statement);

    if (status != SQLITE_ROW) {
        sqlite3_finalize(statement);
        throw std::runtime_error(sqlite3_errmsg(database));
    }

    const auto result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return result;
}

template <std::size_t Size>
std::string blob_literal(const std::array<std::byte, Size>& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result = "X'";
    result.reserve(3 + value.size() * 2);

    for (const auto byte : value) {
        const auto raw = std::to_integer<unsigned int>(byte);
        result.push_back(digits[(raw >> 4U) & 0x0fU]);
        result.push_back(digits[raw & 0x0fU]);
    }

    result.push_back('\'');
    return result;
}

void create_v1_schema(sqlite3* database) {
    execute_raw(database,
                "PRAGMA foreign_keys = ON;"
                "CREATE TABLE devices("
                "device_id BLOB PRIMARY KEY NOT NULL CHECK(length(device_id) = 32),"
                "first_seen_at_ms INTEGER NOT NULL CHECK(first_seen_at_ms >= 0),"
                "last_seen_at_ms INTEGER NOT NULL "
                "CHECK(last_seen_at_ms >= first_seen_at_ms)"
                ") WITHOUT ROWID;"
                "CREATE TABLE networks("
                "network_id BLOB PRIMARY KEY NOT NULL CHECK(length(network_id) = 16),"
                "name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 128),"
                "owner_device_id BLOB NOT NULL CHECK(length(owner_device_id) = 32),"
                "created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),"
                "FOREIGN KEY(owner_device_id) REFERENCES devices(device_id) "
                "ON UPDATE CASCADE ON DELETE RESTRICT"
                ") WITHOUT ROWID;"
                "CREATE TABLE memberships("
                "network_id BLOB NOT NULL CHECK(length(network_id) = 16),"
                "device_id BLOB NOT NULL CHECK(length(device_id) = 32),"
                "role INTEGER NOT NULL CHECK(role IN (0, 1)),"
                "joined_at_ms INTEGER NOT NULL CHECK(joined_at_ms >= 0),"
                "PRIMARY KEY(network_id, device_id),"
                "FOREIGN KEY(network_id) REFERENCES networks(network_id) "
                "ON UPDATE CASCADE ON DELETE CASCADE,"
                "FOREIGN KEY(device_id) REFERENCES devices(device_id) "
                "ON UPDATE CASCADE ON DELETE CASCADE"
                ") WITHOUT ROWID;"
                "CREATE INDEX devices_last_seen_idx ON devices(last_seen_at_ms);"
                "CREATE INDEX networks_owner_idx ON networks(owner_device_id);"
                "CREATE INDEX memberships_device_idx ON memberships(device_id);"
                "CREATE INDEX memberships_network_role_idx "
                "ON memberships(network_id, role);"
                "CREATE UNIQUE INDEX memberships_single_owner_idx "
                "ON memberships(network_id) WHERE role = 1;"
                "CREATE TRIGGER membership_owner_matches_network "
                "BEFORE INSERT ON memberships WHEN NEW.role = 1 AND NOT EXISTS("
                "SELECT 1 FROM networks WHERE network_id = NEW.network_id "
                "AND owner_device_id = NEW.device_id"
                ") BEGIN "
                "SELECT RAISE(ABORT, 'owner membership does not match network owner');"
                "END;"
                "PRAGMA user_version = 1;");
}

void test_schema_and_migration(const std::filesystem::path& directory) {
    const auto path = directory / "nested" / "relay.db";

    {
        lanlink::storage::RelayStore store(path);
        expect(store.database_path() == path, "database path");
        expect(store.schema_version() == lanlink::storage::current_schema_version,
               "schema version");
        expect(store.journal_mode() == "wal", "WAL journal mode");
    }

    expect(std::filesystem::is_regular_file(path), "database file created");
    sqlite3* database = nullptr;

    if (sqlite3_open(path.string().c_str(), &database) != SQLITE_OK) {
        throw std::runtime_error("cannot inspect test database");
    }

    try {
        expect(query_raw_integer(database, "PRAGMA user_version;") ==
                   lanlink::storage::current_schema_version,
               "persisted schema version");
        expect(query_raw_integer(
                   database,
                   "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
                   "AND name IN ('devices', 'networks', 'memberships', "
                   "'network_invitations', 'network_join_requests', "
                   "'network_subnets', 'virtual_ipv4_leases');") == 7,
               "schema tables");
        expect(query_raw_integer(
                   database,
                   "SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' "
                   "AND name IN ('devices_last_seen_idx', 'networks_owner_idx', "
                   "'memberships_device_idx', 'memberships_network_role_idx', "
                   "'memberships_single_owner_idx', 'network_invitations_device_idx', "
                   "'network_join_requests_device_idx', "
                   "'virtual_ipv4_leases_network_address_idx');") == 8,
               "schema indexes");
        expect(query_raw_integer(
                   database,
                   "SELECT COUNT(*) FROM sqlite_master WHERE type = 'trigger' "
                   "AND name IN ('membership_owner_matches_network', "
                   "'invitation_requires_nonmember', "
                   "'invitation_conflicts_with_join_request', "
                   "'join_request_requires_nonmember', "
                   "'join_request_conflicts_with_invitation', "
                   "'membership_clears_pending_access', "
                   "'virtual_ipv4_subnet_insert_guard', "
                   "'virtual_ipv4_subnet_update_guard', "
                   "'virtual_ipv4_subnet_immutable');") == 9,
               "schema triggers");
    } catch (...) {
        sqlite3_close(database);
        throw;
    }

    sqlite3_close(database);

    const auto future_path = directory / "future.db";
    database = nullptr;

    if (sqlite3_open(future_path.string().c_str(), &database) != SQLITE_OK) {
        throw std::runtime_error("cannot create future schema database");
    }

    execute_raw(database, "PRAGMA user_version = 99;");
    sqlite3_close(database);
    expect_error([&future_path] {
        lanlink::storage::RelayStore store(future_path);
    }, "future schema rejected");

    const auto incomplete_path = directory / "incomplete.db";
    database = nullptr;

    if (sqlite3_open(incomplete_path.string().c_str(), &database) != SQLITE_OK) {
        throw std::runtime_error("cannot create incomplete schema database");
    }

    execute_raw(database, "PRAGMA user_version = 1;");
    sqlite3_close(database);
    expect_error([&incomplete_path] {
        lanlink::storage::RelayStore store(incomplete_path);
    }, "incomplete schema rejected");

    const auto migration_path = directory / "migration.db";
    const auto owner = device_id(20);
    const auto invited = device_id(21);
    const auto network = network_id(20);
    database = nullptr;

    if (sqlite3_open(migration_path.string().c_str(), &database) != SQLITE_OK) {
        throw std::runtime_error("cannot create migration database");
    }

    try {
        create_v1_schema(database);
        const auto owner_sql = blob_literal(owner);
        const auto invited_sql = blob_literal(invited);
        const auto network_sql = blob_literal(network);
        execute_raw(database,
                    ("INSERT INTO devices VALUES(" + owner_sql + ", 100, 100);" +
                     "INSERT INTO devices VALUES(" + invited_sql + ", 110, 110);" +
                     "INSERT INTO networks VALUES(" + network_sql +
                     ", 'Legacy LAN', " + owner_sql + ", 120);" +
                     "INSERT INTO memberships VALUES(" + network_sql + ", " + owner_sql +
                     ", 1, 120);")
                        .c_str());
    } catch (...) {
        sqlite3_close(database);
        throw;
    }

    sqlite3_close(database);

    {
        lanlink::storage::RelayStore migrated(migration_path);
        expect(migrated.schema_version() == lanlink::storage::current_schema_version,
               "v1 database migrated");
        const auto state = migrated.load_state();
        expect(state.devices.size() == 2, "migration preserves devices");
        expect(state.networks.size() == 1, "migration preserves networks");
        expect(state.memberships.size() == 1, "migration preserves memberships");
        expect(state.subnets.size() == 1 && state.leases.size() == 1,
               "migration assigns owner subnet and lease");
        expect(state.subnets.size() == 1 && state.leases.size() == 1 &&
                   state.subnets.front().network_address ==
                       lanlink::storage::virtual_ipv4_pool_first &&
                   state.leases.front().address ==
                       lanlink::storage::virtual_ipv4_pool_first + 1,
               "migrated owner receives first virtual IPv4 host");
        expect(state.invitations.empty(), "migration starts without invitations");
        expect(state.join_requests.empty(), "migration starts without join requests");
        expect(migrated.add_invitation(network, invited, 130),
               "migrated database accepts invitations");
    }

    {
        lanlink::storage::RelayStore reopened(migration_path);
        expect(reopened.find_invitation(network, invited) ==
                   lanlink::storage::InvitationRecord{network, invited, 130},
               "migrated invitation persists");
    }
}

void test_persistence_and_relations(const std::filesystem::path& directory) {
    const auto path = directory / "persistent.db";
    const auto owner = device_id(1);
    const auto member = device_id(2);
    const auto missing = device_id(3);
    lanlink::storage::NetworkRecord network;
    network.id = network_id(5);
    network.name = "Panda LAN'); DROP TABLE devices; --";
    network.owner_device_id = owner;
    network.created_at_ms = 1'000;
    lanlink::storage::RelayState expected_state;

    {
        lanlink::storage::RelayStore store(path);
        store.record_device(owner, 200);
        store.record_device(owner, 100);
        store.record_device(owner, 300);
        store.record_device(member, 250);

        const auto stored_owner = store.find_device(owner);
        expect(stored_owner.has_value(), "stored owner device");
        expect(stored_owner && stored_owner->first_seen_at_ms == 100,
               "earliest device timestamp retained");
        expect(stored_owner && stored_owner->last_seen_at_ms == 300,
               "latest device timestamp retained");

        store.create_network(network);
        expect(store.find_network(network.id) == network, "network round trip");
        expect(store.find_subnet(network.id).has_value() &&
                   store.find_subnet(network.id)->network_address ==
                       lanlink::storage::virtual_ipv4_pool_first,
               "network subnet allocated");
        expect(store.find_virtual_ipv4_lease(network.id, owner).has_value() &&
                   lanlink::storage::format_virtual_ipv4(
                       store.find_virtual_ipv4_lease(network.id, owner)->address) ==
                       "10.64.0.1",
               "owner virtual IPv4 allocated");
        const auto owner_memberships = store.list_members(network.id);
        expect(owner_memberships.size() == 1, "owner membership created");
        expect(!owner_memberships.empty() && owner_memberships.front().role ==
                                                  lanlink::storage::MembershipRole::owner,
               "owner membership role");
        expect(!store.remove_member(network.id, owner), "owner membership protected");
        expect(store.add_member(network.id, member, 1'100), "member added");
        expect(!store.add_member(network.id, member, 1'200), "duplicate member ignored");
        expect(store.list_networks_for_device(member) ==
                   std::vector<lanlink::storage::NetworkRecord>{network},
               "networks listed for member");
        expect_error([&store, &network] {
            store.create_network(network);
        }, "duplicate network rejected");
        expect_error([&store, &network, &missing] {
            static_cast<void>(store.add_member(network.id, missing, 1'300));
        }, "unknown device membership rejected");

        expected_state = store.load_state();
        expect(expected_state.devices.size() == 2, "state device count");
        expect(expected_state.networks.size() == 1, "state network count");
        expect(expected_state.memberships.size() == 2, "state membership count");
        expect(expected_state.subnets.size() == 1 && expected_state.leases.size() == 2,
               "virtual subnet and leases included in snapshot");
    }

    {
        lanlink::storage::RelayStore reopened(path);
        expect(reopened.load_state() == expected_state, "state restored after restart");
        expect(reopened.remove_member(network.id, member), "member removed");
        expect(!reopened.remove_member(network.id, member), "missing member removal");
        expect(reopened.add_member(network.id, member, 1'400), "member restored");
        expect(reopened.delete_network(network.id), "network deleted");
        expect(!reopened.delete_network(network.id), "missing network deletion");
        expect(reopened.load_state().networks.empty(), "deleted network absent");
        expect(reopened.load_state().memberships.empty(), "memberships cascade deleted");
        expect(reopened.load_state().subnets.empty() &&
                   reopened.load_state().leases.empty(),
               "virtual IPv4 assignments cascade deleted");
        expect(reopened.load_state().devices.size() == 2, "devices survive network deletion");
    }
}

void test_pending_access_persistence(const std::filesystem::path& directory) {
    const auto path = directory / "pending-access.db";
    const auto owner = device_id(30);
    const auto invited = device_id(31);
    const auto requester = device_id(32);
    const auto first_network_id = network_id(30);
    const auto second_network_id = network_id(31);
    lanlink::storage::NetworkRecord first_network{
        first_network_id, "First LAN", owner, 1'000};
    lanlink::storage::NetworkRecord second_network{
        second_network_id, "Second LAN", owner, 1'100};
    lanlink::storage::RelayState expected_state;

    {
        lanlink::storage::RelayStore store(path);
        store.record_device(owner, 900);
        store.record_device(invited, 901);
        store.record_device(requester, 902);
        store.create_network(first_network);
        store.create_network(second_network);

        expect(store.add_invitation(first_network_id, invited, 1'300),
               "first invitation added");
        expect(store.add_invitation(second_network_id, invited, 1'200),
               "second invitation added");
        expect(!store.add_invitation(first_network_id, invited, 1'400),
               "duplicate invitation ignored");
        expect(store.find_invitation(first_network_id, invited) ==
                   lanlink::storage::InvitationRecord{first_network_id, invited, 1'300},
               "invitation round trip");

        const auto invitations = store.list_invitations_for_device(invited);
        expect(invitations.size() == 2, "device invitation count");
        expect(invitations.size() == 2 && invitations[0].network_id == second_network_id &&
                   invitations[1].network_id == first_network_id,
               "device invitations ordered by time");
        expect(store.list_invitations_for_network(first_network_id) ==
                   std::vector<lanlink::storage::InvitationRecord>{
                       {first_network_id, invited, 1'300}},
               "network invitations listed");

        expect(store.add_join_request(first_network_id, requester, 1'500),
               "first join request added");
        expect(store.add_join_request(second_network_id, requester, 1'400),
               "second join request added");
        expect(!store.add_join_request(first_network_id, requester, 1'600),
               "duplicate join request ignored");
        expect(store.find_join_request(first_network_id, requester) ==
                   lanlink::storage::JoinRequestRecord{first_network_id, requester, 1'500},
               "join request round trip");

        const auto requests = store.list_join_requests_for_device(requester);
        expect(requests.size() == 2, "device join request count");
        expect(requests.size() == 2 && requests[0].network_id == second_network_id &&
                   requests[1].network_id == first_network_id,
               "device join requests ordered by time");
        expect(store.list_join_requests_for_network(first_network_id) ==
                   std::vector<lanlink::storage::JoinRequestRecord>{
                       {first_network_id, requester, 1'500}},
               "network join requests listed");

        expect(!store.add_join_request(first_network_id, invited, 1'700),
               "invitation blocks join request");
        expect(!store.add_invitation(first_network_id, requester, 1'700),
               "join request blocks invitation");
        expect(!store.add_invitation(first_network_id, owner, 1'700),
               "member invitation rejected");
        expect(!store.add_join_request(first_network_id, owner, 1'700),
               "member join request rejected");

        expected_state = store.load_state();
        expect(expected_state.invitations.size() == 2, "state invitation count");
        expect(expected_state.join_requests.size() == 2, "state join request count");
    }

    {
        lanlink::storage::RelayStore reopened(path);
        expect(reopened.load_state() == expected_state, "pending access restored after restart");
        expect(reopened.remove_invitation(second_network_id, invited),
               "invitation removed");
        expect(!reopened.remove_invitation(second_network_id, invited),
               "missing invitation removal");
        expect(reopened.add_invitation(second_network_id, invited, 2'000),
               "removed invitation restored");
        expect(reopened.remove_join_request(second_network_id, requester),
               "join request removed");
        expect(!reopened.remove_join_request(second_network_id, requester),
               "missing join request removal");
        expect(reopened.add_join_request(second_network_id, requester, 2'100),
               "removed join request restored");
        expect(reopened.add_member(first_network_id, invited, 1'800),
               "invited device added as member");
        expect(!reopened.find_invitation(first_network_id, invited).has_value(),
               "membership clears invitation");
        expect(reopened.add_member(first_network_id, requester, 1'900),
               "requesting device added as member");
        expect(!reopened.find_join_request(first_network_id, requester).has_value(),
               "membership clears join request");
        expect(!reopened.remove_invitation(first_network_id, invited),
               "cleared invitation cannot be removed twice");
        expect(!reopened.remove_join_request(first_network_id, requester),
               "cleared join request cannot be removed twice");
        expect(reopened.delete_network(second_network_id),
               "network with pending access deleted");
        expect(reopened.list_invitations_for_device(invited).empty(),
               "network deletion cascades invitations");
        expect(reopened.list_join_requests_for_device(requester).empty(),
               "network deletion cascades join requests");
    }
}

void test_validation(const std::filesystem::path& directory) {
    expect_error([] {
        lanlink::storage::RelayStore store(std::filesystem::path{});
    }, "empty database path rejected");
    expect_error([] {
        lanlink::storage::RelayStore store(":memory:");
    }, "memory database rejected");

    lanlink::storage::RelayStore store(directory / "validation.db");
    lanlink::auth::DeviceId zero_device{};
    lanlink::storage::NetworkId zero_network{};
    const auto owner = device_id(10);
    store.record_device(owner, 10);

    expect_error([&store, &zero_device] {
        store.record_device(zero_device, 1);
    }, "zero device rejected");
    expect_error([&store, &owner] {
        store.record_device(owner, -1);
    }, "negative device timestamp rejected");
    expect_error([&store, &zero_network] {
        static_cast<void>(store.find_network(zero_network));
    }, "zero network rejected");

    lanlink::storage::NetworkRecord invalid;
    invalid.id = network_id(11);
    invalid.owner_device_id = owner;
    invalid.created_at_ms = 20;

    expect_error([&store, &invalid] {
        store.create_network(invalid);
    }, "empty network name rejected");

    invalid.name.assign(129, 'x');
    expect_error([&store, &invalid] {
        store.create_network(invalid);
    }, "long network name rejected");

    invalid.name = "bad\nname";
    expect_error([&store, &invalid] {
        store.create_network(invalid);
    }, "network control character rejected");

    invalid.name = "valid";
    invalid.created_at_ms = -1;
    expect_error([&store, &invalid] {
        store.create_network(invalid);
    }, "negative network timestamp rejected");

    invalid.created_at_ms = 20;
    invalid.owner_device_id = device_id(12);
    expect_error([&store, &invalid] {
        store.create_network(invalid);
    }, "unknown network owner rejected");
    expect(!store.find_network(invalid.id).has_value(), "failed create rolled back");

    const auto candidate = device_id(13);
    store.record_device(candidate, 30);
    invalid.owner_device_id = owner;
    store.create_network(invalid);

    expect_error([&store, &zero_network, &candidate] {
        static_cast<void>(store.add_invitation(zero_network, candidate, 40));
    }, "zero invitation network rejected");
    expect_error([&store, &invalid, &zero_device] {
        static_cast<void>(store.add_invitation(invalid.id, zero_device, 40));
    }, "zero invited device rejected");
    expect_error([&store, &invalid, &candidate] {
        static_cast<void>(store.add_invitation(invalid.id, candidate, -1));
    }, "negative invitation timestamp rejected");
    expect_error([&store, &candidate] {
        static_cast<void>(store.add_invitation(network_id(99), candidate, 40));
    }, "unknown invitation network rejected");
    expect_error([&store, &invalid] {
        static_cast<void>(store.add_invitation(invalid.id, device_id(99), 40));
    }, "unknown invited device rejected");

    expect_error([&store, &zero_network, &candidate] {
        static_cast<void>(store.add_join_request(zero_network, candidate, 40));
    }, "zero join request network rejected");
    expect_error([&store, &invalid, &zero_device] {
        static_cast<void>(store.add_join_request(invalid.id, zero_device, 40));
    }, "zero requesting device rejected");
    expect_error([&store, &invalid, &candidate] {
        static_cast<void>(store.add_join_request(invalid.id, candidate, -1));
    }, "negative join request timestamp rejected");
    expect_error([&store, &candidate] {
        static_cast<void>(store.add_join_request(network_id(99), candidate, 40));
    }, "unknown join request network rejected");
    expect_error([&store, &invalid] {
        static_cast<void>(store.add_join_request(invalid.id, device_id(99), 40));
    }, "unknown requesting device rejected");
}

void test_concurrent_connections(const std::filesystem::path& directory) {
    const auto path = directory / "concurrent.db";
    lanlink::storage::RelayStore first(path);
    lanlink::storage::RelayStore second(path);
    const auto owner = device_id(100);
    lanlink::storage::NetworkRecord network;
    network.id = network_id(100);
    network.name = "Concurrent LAN";
    network.owner_device_id = owner;
    network.created_at_ms = 5'000;
    first.record_device(owner, 5'000);
    first.create_network(network);
    std::atomic_int thread_failures = 0;
    std::vector<std::thread> threads;

    for (unsigned int thread_index = 0; thread_index < 4; ++thread_index) {
        threads.emplace_back([&, thread_index] {
            try {
                auto& store = thread_index % 2U == 0U ? first : second;

                for (unsigned int entry = 0; entry < 20; ++entry) {
                    const auto seed = 1U + thread_index * 20U + entry;
                    const auto id = device_id(seed);
                    store.record_device(id, 6'000 + seed);

                    if (!store.add_member(network.id, id, 7'000 + seed)) {
                        ++thread_failures;
                    }
                }
            } catch (const std::exception&) {
                ++thread_failures;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    expect(thread_failures.load() == 0, "concurrent writes");
    const auto state = first.load_state();
    expect(state.devices.size() == 81, "concurrent device count");
    expect(state.networks.size() == 1, "concurrent network count");
    expect(state.memberships.size() == 81, "concurrent membership count");
    expect(state.leases.size() == 81, "concurrent unique lease count");
}

void test_concurrent_pending_access(const std::filesystem::path& directory) {
    const auto path = directory / "concurrent-pending.db";
    lanlink::storage::RelayStore first(path);
    lanlink::storage::RelayStore second(path);
    const auto owner = device_id(200);
    lanlink::storage::NetworkRecord network;
    network.id = network_id(200);
    network.name = "Concurrent Pending LAN";
    network.owner_device_id = owner;
    network.created_at_ms = 10'000;
    first.record_device(owner, 10'000);
    first.create_network(network);
    std::atomic_int thread_failures = 0;
    std::vector<std::thread> threads;

    for (unsigned int thread_index = 0; thread_index < 4; ++thread_index) {
        threads.emplace_back([&, thread_index] {
            try {
                auto& store = thread_index % 2U == 0U ? first : second;

                for (unsigned int entry = 0; entry < 20; ++entry) {
                    const auto seed = 1U + thread_index * 20U + entry;
                    const auto id = device_id(seed);
                    store.record_device(id, 11'000 + seed);
                    const bool inserted = seed % 2U == 0U
                                              ? store.add_invitation(
                                                    network.id, id, 12'000 + seed)
                                              : store.add_join_request(
                                                    network.id, id, 12'000 + seed);

                    if (!inserted) {
                        ++thread_failures;
                    }
                }
            } catch (const std::exception&) {
                ++thread_failures;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    expect(thread_failures.load() == 0, "concurrent pending writes");
    const auto state = first.load_state();
    expect(state.devices.size() == 81, "concurrent pending device count");
    expect(state.networks.size() == 1, "concurrent pending network count");
    expect(state.memberships.size() == 1, "concurrent pending membership count");
    expect(state.invitations.size() == 40, "concurrent invitation count");
    expect(state.join_requests.size() == 40, "concurrent join request count");
}

}

int main() {
    const auto directory = make_test_directory();

    try {
        test_schema_and_migration(directory);
        test_persistence_and_relations(directory);
        test_pending_access_persistence(directory);
        test_validation(directory);
        test_concurrent_connections(directory);
        test_concurrent_pending_access(directory);
    } catch (const std::exception& error) {
        std::cerr << "unexpected error: " << error.what() << '\n';
        ++failures;
    }

    std::error_code error;
    std::filesystem::remove_all(directory, error);

    if (error) {
        std::cerr << "cleanup failed: " << error.message() << '\n';
        ++failures;
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "all storage tests passed\n";
    return 0;
}
