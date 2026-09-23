#include "lanlink/storage/relay_store.hpp"

#include <sqlite3.h>

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
                   "AND name IN ('devices', 'networks', 'memberships');") == 3,
               "schema tables");
        expect(query_raw_integer(
                   database,
                   "SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' "
                   "AND name IN ('devices_last_seen_idx', 'networks_owner_idx', "
                   "'memberships_device_idx', 'memberships_network_role_idx', "
                   "'memberships_single_owner_idx');") == 5,
               "schema indexes");
        expect(query_raw_integer(
                   database,
                   "SELECT COUNT(*) FROM sqlite_master WHERE type = 'trigger' "
                   "AND name = 'membership_owner_matches_network';") == 1,
               "schema trigger");
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
        expect(reopened.load_state().devices.size() == 2, "devices survive network deletion");
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
}

}

int main() {
    const auto directory = make_test_directory();

    try {
        test_schema_and_migration(directory);
        test_persistence_and_relations(directory);
        test_validation(directory);
        test_concurrent_connections(directory);
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
