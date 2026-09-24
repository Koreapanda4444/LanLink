#include "lanlink/storage/relay_store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lanlink::storage {
namespace {

constexpr std::size_t max_network_name_size = 128;
constexpr int busy_timeout_ms = 5'000;

[[noreturn]] void throw_sqlite(sqlite3* database,
                               const std::string_view operation,
                               const int status) {
    const char* detail = database == nullptr ? nullptr : sqlite3_errmsg(database);
    std::string message(operation);
    message += " failed (" + std::to_string(status) + ")";

    if (detail != nullptr) {
        message += ": ";
        message += detail;
    }

    throw std::runtime_error(message);
}

void execute(sqlite3* database, const char* sql) {
    char* detail = nullptr;
    const int status = sqlite3_exec(database, sql, nullptr, nullptr, &detail);

    if (status == SQLITE_OK) {
        return;
    }

    std::string message = "SQLite statement failed (" + std::to_string(status) + ")";

    if (detail != nullptr) {
        message += ": ";
        message += detail;
        sqlite3_free(detail);
    }

    throw std::runtime_error(message);
}

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database) {
        const int status = sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr);

        if (status != SQLITE_OK) {
            throw_sqlite(database_, "preparing SQLite statement", status);
        }
    }

    ~Statement() {
        sqlite3_finalize(statement_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind_blob(const int index, const void* data, const std::size_t size) {
        if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("SQLite blob is too large");
        }

        const int status = sqlite3_bind_blob(statement_,
                                             index,
                                             data,
                                             static_cast<int>(size),
                                             SQLITE_TRANSIENT);

        if (status != SQLITE_OK) {
            throw_sqlite(database_, "binding SQLite blob", status);
        }
    }

    void bind_text(const int index, const std::string_view value) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("SQLite text is too large");
        }

        const int status = sqlite3_bind_text(statement_,
                                             index,
                                             value.data(),
                                             static_cast<int>(value.size()),
                                             SQLITE_TRANSIENT);

        if (status != SQLITE_OK) {
            throw_sqlite(database_, "binding SQLite text", status);
        }
    }

    void bind_integer(const int index, const std::int64_t value) {
        const int status = sqlite3_bind_int64(statement_, index, value);

        if (status != SQLITE_OK) {
            throw_sqlite(database_, "binding SQLite integer", status);
        }
    }

    [[nodiscard]] bool step_row() {
        const int status = sqlite3_step(statement_);

        if (status == SQLITE_ROW) {
            return true;
        }
        if (status == SQLITE_DONE) {
            return false;
        }

        throw_sqlite(database_, "reading SQLite row", status);
    }

    void step_done() {
        const int status = sqlite3_step(statement_);

        if (status != SQLITE_DONE) {
            throw_sqlite(database_, "executing SQLite statement", status);
        }
    }

    template <std::size_t Size>
    [[nodiscard]] std::array<std::byte, Size> column_blob(const int column) const {
        if (sqlite3_column_type(statement_, column) != SQLITE_BLOB ||
            sqlite3_column_bytes(statement_, column) != static_cast<int>(Size)) {
            throw std::runtime_error("SQLite row contains an invalid identifier");
        }

        const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(statement_, column));

        if (data == nullptr) {
            throw std::runtime_error("SQLite row contains a null identifier");
        }

        std::array<std::byte, Size> result{};
        std::copy_n(data, Size, result.begin());
        return result;
    }

    [[nodiscard]] std::string column_text(const int column) const {
        if (sqlite3_column_type(statement_, column) != SQLITE_TEXT) {
            throw std::runtime_error("SQLite row contains invalid text");
        }

        const auto* data = sqlite3_column_text(statement_, column);
        const int size = sqlite3_column_bytes(statement_, column);

        if (data == nullptr || size < 0) {
            throw std::runtime_error("SQLite row contains null text");
        }

        return {reinterpret_cast<const char*>(data), static_cast<std::size_t>(size)};
    }

    [[nodiscard]] std::int64_t column_integer(const int column) const {
        if (sqlite3_column_type(statement_, column) != SQLITE_INTEGER) {
            throw std::runtime_error("SQLite row contains an invalid integer");
        }

        return sqlite3_column_int64(statement_, column);
    }

private:
    sqlite3* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

class Transaction {
public:
    Transaction(sqlite3* database, const bool write) : database_(database) {
        execute(database_, write ? "BEGIN IMMEDIATE;" : "BEGIN;");
    }

    ~Transaction() {
        if (active_) {
            sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit() {
        execute(database_, "COMMIT;");
        active_ = false;
    }

private:
    sqlite3* database_ = nullptr;
    bool active_ = true;
};

template <std::size_t Size>
bool is_zero(const std::array<std::byte, Size>& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
        return byte == std::byte{0};
    });
}

void require_device_id(const auth::DeviceId& device_id) {
    if (is_zero(device_id)) {
        throw std::invalid_argument("device id must not be zero");
    }
}

void require_network_id(const NetworkId& network_id) {
    if (is_zero(network_id)) {
        throw std::invalid_argument("network id must not be zero");
    }
}

void require_timestamp(const std::int64_t timestamp) {
    if (timestamp < 0) {
        throw std::invalid_argument("timestamp must not be negative");
    }
}

void require_network_name(const std::string_view name) {
    if (name.empty() || name.size() > max_network_name_size) {
        throw std::invalid_argument("network name must contain 1 to 128 bytes");
    }

    const auto invalid = std::any_of(name.begin(), name.end(), [](const char character) {
        const auto value = static_cast<unsigned char>(character);
        return value < 0x20U || value == 0x7fU;
    });

    if (invalid) {
        throw std::invalid_argument("network name contains a control character");
    }
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::int64_t query_integer(sqlite3* database, const char* sql) {
    Statement statement(database, sql);

    if (!statement.step_row()) {
        throw std::runtime_error("SQLite query returned no rows");
    }

    const auto result = statement.column_integer(0);

    if (statement.step_row()) {
        throw std::runtime_error("SQLite query returned multiple rows");
    }

    return result;
}

std::string query_text(sqlite3* database, const char* sql) {
    Statement statement(database, sql);

    if (!statement.step_row()) {
        throw std::runtime_error("SQLite query returned no rows");
    }

    auto result = statement.column_text(0);

    if (statement.step_row()) {
        throw std::runtime_error("SQLite query returned multiple rows");
    }

    return result;
}

DeviceRecord read_device(const Statement& statement) {
    DeviceRecord result;
    result.id = statement.column_blob<auth::device_id_size>(0);
    result.first_seen_at_ms = statement.column_integer(1);
    result.last_seen_at_ms = statement.column_integer(2);
    return result;
}

NetworkRecord read_network(const Statement& statement) {
    NetworkRecord result;
    result.id = statement.column_blob<network_id_size>(0);
    result.name = statement.column_text(1);
    result.owner_device_id = statement.column_blob<auth::device_id_size>(2);
    result.created_at_ms = statement.column_integer(3);
    return result;
}

MembershipRecord read_membership(const Statement& statement) {
    MembershipRecord result;
    result.network_id = statement.column_blob<network_id_size>(0);
    result.device_id = statement.column_blob<auth::device_id_size>(1);
    const auto role = statement.column_integer(2);

    if (role == static_cast<std::int64_t>(MembershipRole::member)) {
        result.role = MembershipRole::member;
    } else if (role == static_cast<std::int64_t>(MembershipRole::owner)) {
        result.role = MembershipRole::owner;
    } else {
        throw std::runtime_error("SQLite row contains an invalid membership role");
    }

    result.joined_at_ms = statement.column_integer(3);
    return result;
}

InvitationRecord read_invitation(const Statement& statement) {
    InvitationRecord result;
    result.network_id = statement.column_blob<network_id_size>(0);
    result.device_id = statement.column_blob<auth::device_id_size>(1);
    result.invited_at_ms = statement.column_integer(2);
    return result;
}

JoinRequestRecord read_join_request(const Statement& statement) {
    JoinRequestRecord result;
    result.network_id = statement.column_blob<network_id_size>(0);
    result.device_id = statement.column_blob<auth::device_id_size>(1);
    result.requested_at_ms = statement.column_integer(2);
    return result;
}

std::vector<DeviceRecord> list_devices(sqlite3* database) {
    Statement statement(database,
                        "SELECT device_id, first_seen_at_ms, last_seen_at_ms "
                        "FROM devices ORDER BY device_id;");
    std::vector<DeviceRecord> result;

    while (statement.step_row()) {
        result.push_back(read_device(statement));
    }

    return result;
}

std::vector<NetworkRecord> list_networks(sqlite3* database) {
    Statement statement(database,
                        "SELECT network_id, name, owner_device_id, created_at_ms "
                        "FROM networks ORDER BY network_id;");
    std::vector<NetworkRecord> result;

    while (statement.step_row()) {
        result.push_back(read_network(statement));
    }

    return result;
}

std::vector<MembershipRecord> list_memberships(sqlite3* database) {
    Statement statement(database,
                        "SELECT network_id, device_id, role, joined_at_ms "
                        "FROM memberships ORDER BY network_id, role DESC, device_id;");
    std::vector<MembershipRecord> result;

    while (statement.step_row()) {
        result.push_back(read_membership(statement));
    }

    return result;
}

std::vector<InvitationRecord> list_invitations(sqlite3* database) {
    Statement statement(database,
                        "SELECT network_id, device_id, invited_at_ms "
                        "FROM network_invitations ORDER BY network_id, device_id;");
    std::vector<InvitationRecord> result;

    while (statement.step_row()) {
        result.push_back(read_invitation(statement));
    }

    return result;
}

std::vector<JoinRequestRecord> list_join_requests(sqlite3* database) {
    Statement statement(database,
                        "SELECT network_id, device_id, requested_at_ms "
                        "FROM network_join_requests ORDER BY network_id, device_id;");
    std::vector<JoinRequestRecord> result;

    while (statement.step_row()) {
        result.push_back(read_join_request(statement));
    }

    return result;
}

}

class RelayStore::Impl {
public:
    explicit Impl(std::filesystem::path path) : path_(std::move(path)) {
        if (path_.empty() || path_ == std::filesystem::path{":memory:"}) {
            throw std::invalid_argument("relay database requires a persistent file path");
        }

        const auto parent = path_.parent_path();

        if (!parent.empty()) {
            std::error_code error;
            std::filesystem::create_directories(parent, error);

            if (error) {
                throw std::filesystem::filesystem_error(
                    "cannot create relay database directory", parent, error);
            }
        }

        const auto encoded_path = path_utf8(path_);
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        const int open_status = sqlite3_open_v2(encoded_path.c_str(), &database_, flags, nullptr);

        if (open_status != SQLITE_OK) {
            const std::string detail = database_ == nullptr ? "unknown error"
                                                            : sqlite3_errmsg(database_);

            if (database_ != nullptr) {
                sqlite3_close_v2(database_);
                database_ = nullptr;
            }

            throw std::runtime_error("cannot open relay database: " + detail);
        }

        try {
            const int extended_status = sqlite3_extended_result_codes(database_, 1);

            if (extended_status != SQLITE_OK) {
                throw_sqlite(database_, "enabling SQLite extended result codes", extended_status);
            }

            const int timeout_status = sqlite3_busy_timeout(database_, busy_timeout_ms);

            if (timeout_status != SQLITE_OK) {
                throw_sqlite(database_, "configuring SQLite busy timeout", timeout_status);
            }

            execute(database_, "PRAGMA foreign_keys = ON;");

            if (query_integer(database_, "PRAGMA foreign_keys;") != 1) {
                throw std::runtime_error("SQLite foreign key enforcement is unavailable");
            }

            auto mode = query_text(database_, "PRAGMA journal_mode = WAL;");
            std::transform(mode.begin(), mode.end(), mode.begin(), [](const char character) {
                if (character >= 'A' && character <= 'Z') {
                    return static_cast<char>(character - 'A' + 'a');
                }

                return character;
            });

            if (mode != "wal") {
                throw std::runtime_error("SQLite WAL journal mode is unavailable");
            }

            execute(database_,
                    "PRAGMA synchronous = NORMAL;"
                    "PRAGMA wal_autocheckpoint = 1000;"
                    "PRAGMA journal_size_limit = 16777216;");
            migrate();
            validate_schema();

            if (query_text(database_, "PRAGMA quick_check;") != "ok") {
                throw std::runtime_error("relay database integrity check failed");
            }
        } catch (...) {
            sqlite3_close_v2(database_);
            database_ = nullptr;
            throw;
        }
    }

    ~Impl() {
        if (database_ != nullptr) {
            sqlite3_close_v2(database_);
        }
    }

    [[nodiscard]] const std::filesystem::path& database_path() const noexcept {
        return path_;
    }

    [[nodiscard]] int schema_version() const {
        std::lock_guard lock(mutex_);
        return static_cast<int>(query_integer(database_, "PRAGMA user_version;"));
    }

    [[nodiscard]] std::string journal_mode() const {
        std::lock_guard lock(mutex_);
        return query_text(database_, "PRAGMA journal_mode;");
    }

    void record_device(const auth::DeviceId& device_id, const std::int64_t seen_at_ms) {
        require_device_id(device_id);
        require_timestamp(seen_at_ms);
        std::lock_guard lock(mutex_);
        Statement statement(
            database_,
            "INSERT INTO devices(device_id, first_seen_at_ms, last_seen_at_ms) "
            "VALUES(?1, ?2, ?2) "
            "ON CONFLICT(device_id) DO UPDATE SET "
            "first_seen_at_ms = MIN(devices.first_seen_at_ms, excluded.first_seen_at_ms), "
            "last_seen_at_ms = MAX(devices.last_seen_at_ms, excluded.last_seen_at_ms);");
        statement.bind_blob(1, device_id.data(), device_id.size());
        statement.bind_integer(2, seen_at_ms);
        statement.step_done();
    }

    [[nodiscard]] std::optional<DeviceRecord> find_device(
        const auth::DeviceId& device_id) const {
        require_device_id(device_id);
        std::lock_guard lock(mutex_);
        Statement statement(database_,
                            "SELECT device_id, first_seen_at_ms, last_seen_at_ms "
                            "FROM devices WHERE device_id = ?1;");
        statement.bind_blob(1, device_id.data(), device_id.size());

        if (!statement.step_row()) {
            return std::nullopt;
        }

        auto result = read_device(statement);

        if (statement.step_row()) {
            throw std::runtime_error("device query returned duplicate rows");
        }

        return result;
    }

    void create_network(const NetworkRecord& network) {
        require_network_id(network.id);
        require_network_name(network.name);
        require_device_id(network.owner_device_id);
        require_timestamp(network.created_at_ms);
        std::lock_guard lock(mutex_);
        Transaction transaction(database_, true);

        {
            Statement statement(
                database_,
                "INSERT INTO networks(network_id, name, owner_device_id, created_at_ms) "
                "VALUES(?1, ?2, ?3, ?4);");
            statement.bind_blob(1, network.id.data(), network.id.size());
            statement.bind_text(2, network.name);
            statement.bind_blob(
                3, network.owner_device_id.data(), network.owner_device_id.size());
            statement.bind_integer(4, network.created_at_ms);
            statement.step_done();
        }

        {
            Statement statement(database_,
                                "INSERT INTO memberships("
                                "network_id, device_id, role, joined_at_ms) "
                                "VALUES(?1, ?2, ?3, ?4);");
            statement.bind_blob(1, network.id.data(), network.id.size());
            statement.bind_blob(
                2, network.owner_device_id.data(), network.owner_device_id.size());
            statement.bind_integer(3, static_cast<std::int64_t>(MembershipRole::owner));
            statement.bind_integer(4, network.created_at_ms);
            statement.step_done();
        }

        transaction.commit();
    }

    [[nodiscard]] std::optional<NetworkRecord> find_network(
        const NetworkId& network_id) const {
        require_network_id(network_id);
        std::lock_guard lock(mutex_);
        Statement statement(database_,
                            "SELECT network_id, name, owner_device_id, created_at_ms "
                            "FROM networks WHERE network_id = ?1;");
        statement.bind_blob(1, network_id.data(), network_id.size());

        if (!statement.step_row()) {
            return std::nullopt;
        }

        auto result = read_network(statement);

        if (statement.step_row()) {
            throw std::runtime_error("network query returned duplicate rows");
        }

        return result;
    }

    [[nodiscard]] bool delete_network(const NetworkId& network_id) {
        require_network_id(network_id);
        std::lock_guard lock(mutex_);
        Statement statement(database_, "DELETE FROM networks WHERE network_id = ?1;");
        statement.bind_blob(1, network_id.data(), network_id.size());
        statement.step_done();
        return sqlite3_changes(database_) == 1;
    }

    [[nodiscard]] bool add_member(const NetworkId& network_id,
                                  const auth::DeviceId& device_id,
                                  const std::int64_t joined_at_ms) {
        require_network_id(network_id);
        require_device_id(device_id);
        require_timestamp(joined_at_ms);
        std::lock_guard lock(mutex_);
        Statement statement(database_,
                            "INSERT OR IGNORE INTO memberships("
                            "network_id, device_id, role, joined_at_ms) "
                            "VALUES(?1, ?2, ?3, ?4);");
        statement.bind_blob(1, network_id.data(), network_id.size());
        statement.bind_blob(2, device_id.data(), device_id.size());
        statement.bind_integer(3, static_cast<std::int64_t>(MembershipRole::member));
        statement.bind_integer(4, joined_at_ms);
        statement.step_done();
        return sqlite3_changes(database_) == 1;
    }

    [[nodiscard]] bool remove_member(const NetworkId& network_id,
                                     const auth::DeviceId& device_id) {
        require_network_id(network_id);
        require_device_id(device_id);
        std::lock_guard lock(mutex_);
        Statement statement(database_,
                            "DELETE FROM memberships "
                            "WHERE network_id = ?1 AND device_id = ?2 AND role = ?3;");
        statement.bind_blob(1, network_id.data(), network_id.size());
        statement.bind_blob(2, device_id.data(), device_id.size());
        statement.bind_integer(3, static_cast<std::int64_t>(MembershipRole::member));
        statement.step_done();
        return sqlite3_changes(database_) == 1;
    }

    [[nodiscard]] std::vector<MembershipRecord> list_members(
        const NetworkId& network_id) const {
        require_network_id(network_id);
        std::lock_guard lock(mutex_);
        Statement statement(database_,
                            "SELECT network_id, device_id, role, joined_at_ms "
                            "FROM memberships WHERE network_id = ?1 "
                            "ORDER BY role DESC, device_id;");
        statement.bind_blob(1, network_id.data(), network_id.size());
        std::vector<MembershipRecord> result;

        while (statement.step_row()) {
            result.push_back(read_membership(statement));
        }

        return result;
    }

    [[nodiscard]] std::vector<NetworkRecord> list_networks_for_device(
        const auth::DeviceId& device_id) const {
        require_device_id(device_id);
        std::lock_guard lock(mutex_);
        Statement statement(database_,
                            "SELECT n.network_id, n.name, n.owner_device_id, n.created_at_ms "
                            "FROM networks AS n "
                            "INNER JOIN memberships AS m ON m.network_id = n.network_id "
                            "WHERE m.device_id = ?1 "
                            "ORDER BY n.created_at_ms, n.network_id;");
        statement.bind_blob(1, device_id.data(), device_id.size());
        std::vector<NetworkRecord> result;

        while (statement.step_row()) {
            result.push_back(read_network(statement));
        }

        return result;
    }

    [[nodiscard]] bool add_invitation(const NetworkId& network_id,
                                      const auth::DeviceId& device_id,
                                      const std::int64_t invited_at_ms) {
        require_network_id(network_id);
        require_device_id(device_id);
        require_timestamp(invited_at_ms);
        std::lock_guard lock(mutex_);
        Statement statement(
            database_,
            "INSERT INTO network_invitations(network_id, device_id, invited_at_ms) "
            "SELECT ?1, ?2, ?3 "
            "WHERE NOT EXISTS(SELECT 1 FROM memberships "
            "WHERE network_id = ?1 AND device_id = ?2) "
            "AND NOT EXISTS(SELECT 1 FROM network_join_requests "
            "WHERE network_id = ?1 AND device_id = ?2) "
            "ON CONFLICT(network_id, device_id) DO NOTHING;");
        statement.bind_blob(1, network_id.data(), network_id.size());
        statement.bind_blob(2, device_id.data(), device_id.size());
        statement.bind_integer(3, invited_at_ms);
        statement.step_done();
        return sqlite3_changes(database_) == 1;
    }

    [[nodiscard]] bool remove_invitation(const NetworkId& network_id,
                                         const auth::DeviceId& device_id) {
        require_network_id(network_id);
        require_device_id(device_id);
        std::lock_guard lock(mutex_);
        Statement statement(database_,
                            "DELETE FROM network_invitations "
                            "WHERE network_id = ?1 AND device_id = ?2;");
        statement.bind_blob(1, network_id.data(), network_id.size());
        statement.bind_blob(2, device_id.data(), device_id.size());
        statement.step_done();
        return sqlite3_changes(database_) == 1;
    }

    [[nodiscard]] std::optional<InvitationRecord> find_invitation(
        const NetworkId& network_id,
        const auth::DeviceId& device_id) const {
        require_network_id(network_id);
        require_device_id(device_id);
        std::lock_guard lock(mutex_);
        Statement statement(database_,
                            "SELECT network_id, device_id, invited_at_ms "
                            "FROM network_invitations "
                            "WHERE network_id = ?1 AND device_id = ?2;");
        statement.bind_blob(1, network_id.data(), network_id.size());
        statement.bind_blob(2, device_id.data(), device_id.size());

        if (!statement.step_row()) {
            return std::nullopt;
        }

        auto result = read_invitation(statement);

        if (statement.step_row()) {
            throw std::runtime_error("invitation query returned duplicate rows");
        }

        return result;
    }

    [[nodiscard]] std::vector<InvitationRecord> list_invitations_for_network(
        const NetworkId& network_id) const {
        require_network_id(network_id);
        std::lock_guard lock(mutex_);
        Statement statement(database_,
                            "SELECT network_id, device_id, invited_at_ms "
                            "FROM network_invitations WHERE network_id = ?1 "
                            "ORDER BY invited_at_ms, device_id;");
        statement.bind_blob(1, network_id.data(), network_id.size());
        std::vector<InvitationRecord> result;

        while (statement.step_row()) {
            result.push_back(read_invitation(statement));
        }

        return result;
    }

    [[nodiscard]] std::vector<InvitationRecord> list_invitations_for_device(
        const auth::DeviceId& device_id) const {
        require_device_id(device_id);
        std::lock_guard lock(mutex_);
        Statement statement(database_,
                            "SELECT network_id, device_id, invited_at_ms "
                            "FROM network_invitations WHERE device_id = ?1 "
                            "ORDER BY invited_at_ms, network_id;");
        statement.bind_blob(1, device_id.data(), device_id.size());
        std::vector<InvitationRecord> result;

        while (statement.step_row()) {
            result.push_back(read_invitation(statement));
        }

        return result;
    }

    [[nodiscard]] bool add_join_request(const NetworkId& network_id,
                                        const auth::DeviceId& device_id,
                                        const std::int64_t requested_at_ms) {
        require_network_id(network_id);
        require_device_id(device_id);
        require_timestamp(requested_at_ms);
        std::lock_guard lock(mutex_);
        Statement statement(
            database_,
            "INSERT INTO network_join_requests(network_id, device_id, requested_at_ms) "
            "SELECT ?1, ?2, ?3 "
            "WHERE NOT EXISTS(SELECT 1 FROM memberships "
            "WHERE network_id = ?1 AND device_id = ?2) "
            "AND NOT EXISTS(SELECT 1 FROM network_invitations "
            "WHERE network_id = ?1 AND device_id = ?2) "
            "ON CONFLICT(network_id, device_id) DO NOTHING;");
        statement.bind_blob(1, network_id.data(), network_id.size());
        statement.bind_blob(2, device_id.data(), device_id.size());
        statement.bind_integer(3, requested_at_ms);
        statement.step_done();
        return sqlite3_changes(database_) == 1;
    }

    [[nodiscard]] bool remove_join_request(const NetworkId& network_id,
                                           const auth::DeviceId& device_id) {
        require_network_id(network_id);
        require_device_id(device_id);
        std::lock_guard lock(mutex_);
        Statement statement(database_,
                            "DELETE FROM network_join_requests "
                            "WHERE network_id = ?1 AND device_id = ?2;");
        statement.bind_blob(1, network_id.data(), network_id.size());
        statement.bind_blob(2, device_id.data(), device_id.size());
        statement.step_done();
        return sqlite3_changes(database_) == 1;
    }

    [[nodiscard]] std::optional<JoinRequestRecord> find_join_request(
        const NetworkId& network_id,
        const auth::DeviceId& device_id) const {
        require_network_id(network_id);
        require_device_id(device_id);
        std::lock_guard lock(mutex_);
        Statement statement(database_,
                            "SELECT network_id, device_id, requested_at_ms "
                            "FROM network_join_requests "
                            "WHERE network_id = ?1 AND device_id = ?2;");
        statement.bind_blob(1, network_id.data(), network_id.size());
        statement.bind_blob(2, device_id.data(), device_id.size());

        if (!statement.step_row()) {
            return std::nullopt;
        }

        auto result = read_join_request(statement);

        if (statement.step_row()) {
            throw std::runtime_error("join request query returned duplicate rows");
        }

        return result;
    }

    [[nodiscard]] std::vector<JoinRequestRecord> list_join_requests_for_network(
        const NetworkId& network_id) const {
        require_network_id(network_id);
        std::lock_guard lock(mutex_);
        Statement statement(database_,
                            "SELECT network_id, device_id, requested_at_ms "
                            "FROM network_join_requests WHERE network_id = ?1 "
                            "ORDER BY requested_at_ms, device_id;");
        statement.bind_blob(1, network_id.data(), network_id.size());
        std::vector<JoinRequestRecord> result;

        while (statement.step_row()) {
            result.push_back(read_join_request(statement));
        }

        return result;
    }

    [[nodiscard]] std::vector<JoinRequestRecord> list_join_requests_for_device(
        const auth::DeviceId& device_id) const {
        require_device_id(device_id);
        std::lock_guard lock(mutex_);
        Statement statement(database_,
                            "SELECT network_id, device_id, requested_at_ms "
                            "FROM network_join_requests WHERE device_id = ?1 "
                            "ORDER BY requested_at_ms, network_id;");
        statement.bind_blob(1, device_id.data(), device_id.size());
        std::vector<JoinRequestRecord> result;

        while (statement.step_row()) {
            result.push_back(read_join_request(statement));
        }

        return result;
    }

    [[nodiscard]] RelayState load_state() const {
        std::lock_guard lock(mutex_);
        Transaction transaction(database_, false);
        RelayState result;
        result.devices = list_devices(database_);
        result.networks = list_networks(database_);
        result.memberships = list_memberships(database_);
        result.invitations = list_invitations(database_);
        result.join_requests = list_join_requests(database_);
        transaction.commit();
        return result;
    }

private:
    void migrate() {
        auto version = query_integer(database_, "PRAGMA user_version;");

        if (version > current_schema_version) {
            throw std::runtime_error("relay database schema is newer than this LanLink build");
        }

        if (version == 0) {
            migrate_to_v1();
            version = 1;
        }

        if (version == 1) {
            validate_schema_v1();
            migrate_to_v2();
            version = 2;
        }

        if (version != current_schema_version) {
            throw std::runtime_error("relay database has an unsupported schema version");
        }
    }

    void migrate_to_v1() {
        Transaction transaction(database_, true);
        execute(database_,
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
                "SELECT 1 FROM networks "
                "WHERE network_id = NEW.network_id "
                "AND owner_device_id = NEW.device_id"
                ") BEGIN "
                "SELECT RAISE(ABORT, 'owner membership does not match network owner');"
                "END;"
                "PRAGMA user_version = 1;");
        transaction.commit();
    }

    void migrate_to_v2() {
        Transaction transaction(database_, true);
        execute(database_,
                "CREATE TABLE network_invitations("
                "network_id BLOB NOT NULL CHECK(length(network_id) = 16),"
                "device_id BLOB NOT NULL CHECK(length(device_id) = 32),"
                "invited_at_ms INTEGER NOT NULL CHECK(invited_at_ms >= 0),"
                "PRIMARY KEY(network_id, device_id),"
                "FOREIGN KEY(network_id) REFERENCES networks(network_id) "
                "ON UPDATE CASCADE ON DELETE CASCADE,"
                "FOREIGN KEY(device_id) REFERENCES devices(device_id) "
                "ON UPDATE CASCADE ON DELETE CASCADE"
                ") WITHOUT ROWID;"
                "CREATE TABLE network_join_requests("
                "network_id BLOB NOT NULL CHECK(length(network_id) = 16),"
                "device_id BLOB NOT NULL CHECK(length(device_id) = 32),"
                "requested_at_ms INTEGER NOT NULL CHECK(requested_at_ms >= 0),"
                "PRIMARY KEY(network_id, device_id),"
                "FOREIGN KEY(network_id) REFERENCES networks(network_id) "
                "ON UPDATE CASCADE ON DELETE CASCADE,"
                "FOREIGN KEY(device_id) REFERENCES devices(device_id) "
                "ON UPDATE CASCADE ON DELETE CASCADE"
                ") WITHOUT ROWID;"
                "CREATE INDEX network_invitations_device_idx "
                "ON network_invitations(device_id, invited_at_ms, network_id);"
                "CREATE INDEX network_join_requests_device_idx "
                "ON network_join_requests(device_id, requested_at_ms, network_id);"
                "CREATE TRIGGER invitation_requires_nonmember "
                "BEFORE INSERT ON network_invitations WHEN EXISTS("
                "SELECT 1 FROM memberships WHERE network_id = NEW.network_id "
                "AND device_id = NEW.device_id"
                ") BEGIN "
                "SELECT RAISE(ABORT, 'network member cannot have an invitation');"
                "END;"
                "CREATE TRIGGER invitation_conflicts_with_join_request "
                "BEFORE INSERT ON network_invitations WHEN EXISTS("
                "SELECT 1 FROM network_join_requests WHERE network_id = NEW.network_id "
                "AND device_id = NEW.device_id"
                ") BEGIN "
                "SELECT RAISE(ABORT, 'join request already exists for invitation');"
                "END;"
                "CREATE TRIGGER join_request_requires_nonmember "
                "BEFORE INSERT ON network_join_requests WHEN EXISTS("
                "SELECT 1 FROM memberships WHERE network_id = NEW.network_id "
                "AND device_id = NEW.device_id"
                ") BEGIN "
                "SELECT RAISE(ABORT, 'network member cannot have a join request');"
                "END;"
                "CREATE TRIGGER join_request_conflicts_with_invitation "
                "BEFORE INSERT ON network_join_requests WHEN EXISTS("
                "SELECT 1 FROM network_invitations WHERE network_id = NEW.network_id "
                "AND device_id = NEW.device_id"
                ") BEGIN "
                "SELECT RAISE(ABORT, 'invitation already exists for join request');"
                "END;"
                "CREATE TRIGGER membership_clears_pending_access "
                "AFTER INSERT ON memberships BEGIN "
                "DELETE FROM network_invitations WHERE network_id = NEW.network_id "
                "AND device_id = NEW.device_id;"
                "DELETE FROM network_join_requests WHERE network_id = NEW.network_id "
                "AND device_id = NEW.device_id;"
                "END;"
                "PRAGMA user_version = 2;");
        transaction.commit();
    }

    void validate_schema_v1() {
        const auto tables = query_integer(
            database_,
            "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' "
            "AND name IN ('devices', 'networks', 'memberships');");
        const auto indexes = query_integer(
            database_,
            "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'index' "
            "AND name IN ('devices_last_seen_idx', 'networks_owner_idx', "
            "'memberships_device_idx', 'memberships_network_role_idx', "
            "'memberships_single_owner_idx');");
        const auto triggers = query_integer(
            database_,
            "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'trigger' "
            "AND name = 'membership_owner_matches_network';");

        if (tables != 3 || indexes != 5 || triggers != 1) {
            throw std::runtime_error("relay database schema is incomplete");
        }
    }

    void validate_schema() {
        validate_schema_v1();
        const auto tables = query_integer(
            database_,
            "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' "
            "AND name IN ('network_invitations', 'network_join_requests');");
        const auto indexes = query_integer(
            database_,
            "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'index' "
            "AND name IN ('network_invitations_device_idx', "
            "'network_join_requests_device_idx');");
        const auto triggers = query_integer(
            database_,
            "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'trigger' "
            "AND name IN ('invitation_requires_nonmember', "
            "'invitation_conflicts_with_join_request', "
            "'join_request_requires_nonmember', "
            "'join_request_conflicts_with_invitation', "
            "'membership_clears_pending_access');");

        if (tables != 2 || indexes != 2 || triggers != 5) {
            throw std::runtime_error("relay database schema is incomplete");
        }
    }

    std::filesystem::path path_;
    sqlite3* database_ = nullptr;
    mutable std::mutex mutex_;
};

RelayStore::RelayStore(std::filesystem::path database_path)
    : impl_(std::make_unique<Impl>(std::move(database_path))) {
}

RelayStore::~RelayStore() = default;

const std::filesystem::path& RelayStore::database_path() const noexcept {
    return impl_->database_path();
}

int RelayStore::schema_version() const {
    return impl_->schema_version();
}

std::string RelayStore::journal_mode() const {
    return impl_->journal_mode();
}

void RelayStore::record_device(const auth::DeviceId& device_id, const std::int64_t seen_at_ms) {
    impl_->record_device(device_id, seen_at_ms);
}

std::optional<DeviceRecord> RelayStore::find_device(const auth::DeviceId& device_id) const {
    return impl_->find_device(device_id);
}

void RelayStore::create_network(const NetworkRecord& network) {
    impl_->create_network(network);
}

std::optional<NetworkRecord> RelayStore::find_network(const NetworkId& network_id) const {
    return impl_->find_network(network_id);
}

bool RelayStore::delete_network(const NetworkId& network_id) {
    return impl_->delete_network(network_id);
}

bool RelayStore::add_member(const NetworkId& network_id,
                            const auth::DeviceId& device_id,
                            const std::int64_t joined_at_ms) {
    return impl_->add_member(network_id, device_id, joined_at_ms);
}

bool RelayStore::remove_member(const NetworkId& network_id,
                               const auth::DeviceId& device_id) {
    return impl_->remove_member(network_id, device_id);
}

std::vector<MembershipRecord> RelayStore::list_members(const NetworkId& network_id) const {
    return impl_->list_members(network_id);
}

std::vector<NetworkRecord> RelayStore::list_networks_for_device(
    const auth::DeviceId& device_id) const {
    return impl_->list_networks_for_device(device_id);
}

bool RelayStore::add_invitation(const NetworkId& network_id,
                                const auth::DeviceId& device_id,
                                const std::int64_t invited_at_ms) {
    return impl_->add_invitation(network_id, device_id, invited_at_ms);
}

bool RelayStore::remove_invitation(const NetworkId& network_id,
                                   const auth::DeviceId& device_id) {
    return impl_->remove_invitation(network_id, device_id);
}

std::optional<InvitationRecord> RelayStore::find_invitation(
    const NetworkId& network_id,
    const auth::DeviceId& device_id) const {
    return impl_->find_invitation(network_id, device_id);
}

std::vector<InvitationRecord> RelayStore::list_invitations_for_network(
    const NetworkId& network_id) const {
    return impl_->list_invitations_for_network(network_id);
}

std::vector<InvitationRecord> RelayStore::list_invitations_for_device(
    const auth::DeviceId& device_id) const {
    return impl_->list_invitations_for_device(device_id);
}

bool RelayStore::add_join_request(const NetworkId& network_id,
                                  const auth::DeviceId& device_id,
                                  const std::int64_t requested_at_ms) {
    return impl_->add_join_request(network_id, device_id, requested_at_ms);
}

bool RelayStore::remove_join_request(const NetworkId& network_id,
                                     const auth::DeviceId& device_id) {
    return impl_->remove_join_request(network_id, device_id);
}

std::optional<JoinRequestRecord> RelayStore::find_join_request(
    const NetworkId& network_id,
    const auth::DeviceId& device_id) const {
    return impl_->find_join_request(network_id, device_id);
}

std::vector<JoinRequestRecord> RelayStore::list_join_requests_for_network(
    const NetworkId& network_id) const {
    return impl_->list_join_requests_for_network(network_id);
}

std::vector<JoinRequestRecord> RelayStore::list_join_requests_for_device(
    const auth::DeviceId& device_id) const {
    return impl_->list_join_requests_for_device(device_id);
}

RelayState RelayStore::load_state() const {
    return impl_->load_state();
}

}
