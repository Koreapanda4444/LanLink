#include "lanlink/auth/handshake.hpp"
#include "lanlink/auth/identity.hpp"
#include "lanlink/protocol/codec.hpp"
#include "lanlink/protocol/stream_decoder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
                      ("lanlink-auth-tests-" + std::to_string(suffix));
    std::filesystem::create_directories(path);
    return path;
}

void write_token(const std::filesystem::path& path, const std::string_view token) {
    std::ofstream output(path, std::ios::binary);
    output << token << '\n';

    if (!output) {
        throw std::runtime_error("cannot write test token");
    }
}

#ifdef _WIN32
std::vector<std::byte> read_binary(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);

    if (!input) {
        throw std::runtime_error("cannot read test file");
    }

    const auto size = std::filesystem::file_size(path);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    if (static_cast<std::size_t>(input.gcount()) != bytes.size()) {
        throw std::runtime_error("cannot read complete test file");
    }

    return bytes;
}

void write_binary(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));

    if (!output) {
        throw std::runtime_error("cannot write test file");
    }
}
#endif

bool has_temporary_identity(const std::filesystem::path& directory,
                            const std::filesystem::path& identity) {
    const auto prefix = identity.filename().string() + ".tmp-";

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().starts_with(prefix)) {
            return true;
        }
    }

    return false;
}

void test_identity(const std::filesystem::path& directory) {
    const auto path = directory / "device.identity";
    auto first = lanlink::auth::DeviceIdentity::load_or_create(path);
    const auto first_id = first.device_id_hex();
    const std::vector<std::byte> message{
        std::byte{1},
        std::byte{2},
        std::byte{3},
    };
    const auto signature = first.sign(message);

    expect(std::filesystem::exists(path), "identity file created");
#ifdef _WIN32
    const auto stored = read_binary(path);
    expect(stored.size() > 9, "protected identity file size");
    expect(stored.size() > 4 && stored[4] == std::byte{2}, "protected identity version");
#else
    expect(std::filesystem::file_size(path) == 37, "identity file size");
    const auto unsafe_permissions = std::filesystem::perms::group_all |
                                    std::filesystem::perms::others_all;
    expect((std::filesystem::status(path).permissions() & unsafe_permissions) ==
               std::filesystem::perms::none,
           "identity file permissions");
#endif
    expect(!has_temporary_identity(directory, path), "identity temporary file removed");
    expect(first_id.size() == lanlink::auth::device_id_size * 2, "device id length");
    expect(lanlink::auth::verify_signature(first.public_key(), message, signature),
           "identity signature");

    auto altered = message;
    altered.front() = std::byte{9};
    expect(!lanlink::auth::verify_signature(first.public_key(), altered, signature),
           "altered signature rejected");

    auto second = lanlink::auth::DeviceIdentity::load_or_create(path);
    expect(second.device_id_hex() == first_id, "identity remains stable");

#ifndef _WIN32
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_read |
                                     std::filesystem::perms::others_read,
                                 std::filesystem::perm_options::replace);
    auto repaired = lanlink::auth::DeviceIdentity::load_or_create(path);
    expect(repaired.device_id_hex() == first_id, "permission repair preserves identity");
    expect((std::filesystem::status(path).permissions() & unsafe_permissions) ==
               std::filesystem::perms::none,
           "unsafe identity permissions repaired");
#endif

    expect_error([&directory] {
        const auto invalid = directory / "invalid.identity";
        std::ofstream output(invalid, std::ios::binary);
        output << "invalid";
        output.close();
        static_cast<void>(lanlink::auth::DeviceIdentity::load_or_create(invalid));
    }, "invalid identity rejected");
}

#ifdef _WIN32
void test_legacy_identity_migration(const std::filesystem::path& directory) {
    constexpr std::array<std::byte, 4> magic{
        std::byte{'L'},
        std::byte{'L'},
        std::byte{'I'},
        std::byte{'D'},
    };
    std::array<std::byte, 37> legacy{};
    std::copy(magic.begin(), magic.end(), legacy.begin());
    legacy[4] = std::byte{1};

    for (std::size_t index = 5; index < legacy.size(); ++index) {
        legacy[index] = std::byte{static_cast<unsigned char>(index)};
    }

    const auto path = directory / "legacy.identity";
    write_binary(path, legacy);
    auto migrated = lanlink::auth::DeviceIdentity::load_or_create(path);
    const auto device_id = migrated.device_id_hex();
    auto stored = read_binary(path);

    expect(stored.size() > 9, "legacy identity replaced by protected identity");
    expect(stored.size() > 4 && stored[4] == std::byte{2}, "legacy identity migrated");
    expect(!has_temporary_identity(directory, path), "migration temporary file removed");

    auto reloaded = lanlink::auth::DeviceIdentity::load_or_create(path);
    expect(reloaded.device_id_hex() == device_id, "migration preserves device identity");

    stored.back() = stored.back() ^ std::byte{1};
    write_binary(path, stored);
    expect_error([&path] {
        static_cast<void>(lanlink::auth::DeviceIdentity::load_or_create(path));
    }, "corrupted protected identity rejected");
}
#endif

void test_token(const std::filesystem::path& directory) {
    constexpr std::string_view secret = "0123456789abcdef0123456789abcdef";
    const auto path = directory / "auth.token";
    write_token(path, secret);
    auto loaded = lanlink::auth::AuthToken::load(path);
    auto direct = lanlink::auth::AuthToken::from_secret(secret);
    const std::vector<std::byte> message{
        std::byte{0x11},
        std::byte{0x22},
    };
    const auto proof = loaded.proof(message);

    expect(direct.verify(message, proof), "loaded token matches direct token");

    auto altered = message;
    altered.front() = std::byte{0x33};
    expect(!direct.verify(altered, proof), "altered token proof rejected");

    expect_error([] {
        static_cast<void>(lanlink::auth::AuthToken::from_secret("short"));
    }, "short token rejected");
}

void test_payloads() {
    lanlink::auth::ClientHelloPayload hello;
    hello.public_key.fill(std::byte{0x11});
    hello.client_nonce.fill(std::byte{0x22});
    expect(lanlink::auth::decode_client_hello(lanlink::auth::encode_client_hello(hello)) == hello,
           "client hello round trip");

    lanlink::auth::ServerChallengePayload challenge;
    challenge.server_nonce.fill(std::byte{0x33});
    expect(lanlink::auth::decode_server_challenge(
               lanlink::auth::encode_server_challenge(challenge)) == challenge,
           "server challenge round trip");

    lanlink::auth::ClientProofPayload proof;
    proof.signature.fill(std::byte{0x44});
    proof.token_proof.fill(std::byte{0x55});
    expect(lanlink::auth::decode_client_proof(lanlink::auth::encode_client_proof(proof)) == proof,
           "client proof round trip");

    lanlink::auth::AuthResultPayload result{true};
    result.session_id.fill(std::byte{0x66});
    expect(lanlink::auth::decode_auth_result(lanlink::auth::encode_auth_result(result)) == result,
           "auth result round trip");

    const lanlink::auth::AuthResultPayload rejected{};
    expect(lanlink::auth::decode_auth_result(lanlink::auth::encode_auth_result(rejected)) ==
               rejected,
           "rejected auth result round trip");

    expect_error([] {
        static_cast<void>(lanlink::auth::decode_client_hello({std::byte{0}}));
    }, "short client hello rejected");
    expect_error([] {
        static_cast<void>(lanlink::auth::decode_auth_result({std::byte{2}}));
    }, "invalid auth result rejected");
    expect_error([] {
        static_cast<void>(
            lanlink::auth::encode_auth_result(lanlink::auth::AuthResultPayload{true}));
    }, "accepted result requires session id");
    expect_error([] {
        lanlink::auth::AuthResultPayload invalid;
        invalid.session_id.front() = std::byte{1};
        static_cast<void>(lanlink::auth::encode_auth_result(invalid));
    }, "rejected result forbids session id");
}

void test_successful_handshake(const std::filesystem::path& directory) {
    auto identity = lanlink::auth::DeviceIdentity::load_or_create(directory / "handshake.identity");
    auto client_token = lanlink::auth::AuthToken::from_secret(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    auto server_token = lanlink::auth::AuthToken::from_secret(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    lanlink::auth::ClientHandshake client(identity, client_token);
    constexpr std::uintptr_t connection_binding = 0x1234U;
    const auto now = lanlink::auth::ServerHandshake::Clock::now();
    lanlink::auth::ServerHandshake server(server_token,
                                          connection_binding,
                                          now + std::chrono::seconds{10});

    const auto hello = client.begin(71);
    const auto challenge = server.handle_hello(hello, now);
    const auto proof = client.handle_challenge(challenge);
    const auto result = server.handle_proof(proof, now);

    expect(client.handle_result(result), "client accepts auth result");
    expect(client.authenticated(), "client authenticated state");
    expect(server.authenticated(), "server authenticated state");
    expect(server.device_id_hex() == identity.device_id_hex(), "server device id");
    expect(client.session_id().has_value(), "client session id assigned");
    expect(client.session_id() == std::optional<lanlink::auth::SessionId>{server.session_id()},
           "client and server session id match");
    expect(server.bound_to(connection_binding, server.device_id(), server.session_id()),
           "session bound to connection and device");
    expect(!server.bound_to(connection_binding + 1U, server.device_id(), server.session_id()),
           "session rejects wrong connection binding");

    expect_error([&client] {
        static_cast<void>(client.begin(72));
    }, "handshake cannot restart");
    expect_error([&client, &result] {
        static_cast<void>(client.handle_result(result));
    }, "client rejects duplicate auth result");
    expect_error([&server, &proof, now] {
        static_cast<void>(server.handle_proof(proof, now));
    }, "server rejects duplicate proof");

    client.close();
    server.close();
    expect(!client.session_id().has_value(), "client session cleared on close");
    expect(!server.authenticated(), "server session cleared on close");
    expect_error([&server] {
        static_cast<void>(server.session_id());
    }, "closed server session id unavailable");
}

void test_rejected_handshake(const std::filesystem::path& directory) {
    auto identity = lanlink::auth::DeviceIdentity::load_or_create(directory / "rejected.identity");
    auto client_token = lanlink::auth::AuthToken::from_secret(
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    auto server_token = lanlink::auth::AuthToken::from_secret(
        "cccccccccccccccccccccccccccccccc");
    lanlink::auth::ClientHandshake client(identity, client_token);
    const auto now = lanlink::auth::ServerHandshake::Clock::now();
    lanlink::auth::ServerHandshake server(server_token, 0x2222U, now + std::chrono::seconds{10});

    const auto hello = client.begin(19);
    const auto challenge = server.handle_hello(hello, now);
    const auto proof = client.handle_challenge(challenge);
    const auto result = server.handle_proof(proof, now);

    expect(!client.handle_result(result), "wrong token rejected by client");
    expect(!server.authenticated(), "wrong token not authenticated");
    expect(server.rejected(), "server rejected state");
    expect(!client.session_id().has_value(), "rejected client has no session id");
}

void test_strict_state_timeout_and_replay(const std::filesystem::path& directory) {
    auto identity =
        lanlink::auth::DeviceIdentity::load_or_create(directory / "strict-state.identity");
    auto client_token = lanlink::auth::AuthToken::from_secret(
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
    auto server_token = lanlink::auth::AuthToken::from_secret(
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
    const auto now = lanlink::auth::ServerHandshake::Clock::now();
    lanlink::auth::ClientHandshake client(identity, client_token);
    const auto hello = client.begin(83);

    lanlink::auth::ServerHandshake first(server_token, 0x3001U, now + std::chrono::seconds{10});
    const auto first_challenge = first.handle_hello(hello, now);
    const auto first_proof = client.handle_challenge(first_challenge);

    lanlink::auth::ServerHandshake wrong_order(server_token,
                                               0x3002U,
                                               now + std::chrono::seconds{10});
    expect_error([&wrong_order, &first_proof, now] {
        static_cast<void>(wrong_order.handle_proof(first_proof, now));
    }, "proof before hello rejected");

    lanlink::auth::ServerHandshake duplicate_hello(server_token,
                                                   0x3003U,
                                                   now + std::chrono::seconds{10});
    static_cast<void>(duplicate_hello.handle_hello(hello, now));
    expect_error([&duplicate_hello, &hello, now] {
        static_cast<void>(duplicate_hello.handle_hello(hello, now));
    }, "duplicate hello rejected");

    lanlink::auth::ServerHandshake replay(server_token,
                                          0x3004U,
                                          now + std::chrono::seconds{10});
    static_cast<void>(replay.handle_hello(hello, now));
    const auto replay_result = replay.handle_proof(first_proof, now);
    expect(!lanlink::auth::decode_auth_result(replay_result.payload).accepted,
           "proof replay rejected by fresh challenge");
    expect(replay.rejected(), "replayed session enters rejected state");

    lanlink::auth::ServerHandshake timeout(server_token,
                                           0x3005U,
                                           now + std::chrono::milliseconds{5});
    static_cast<void>(timeout.handle_hello(hello, now));
    expect(timeout.expire(now + std::chrono::milliseconds{5}),
           "authentication deadline expires");
    expect(timeout.timed_out(), "timed out session state");
    expect_error([&timeout, &first_proof, now] {
        static_cast<void>(
            timeout.handle_proof(first_proof, now + std::chrono::milliseconds{6}));
    }, "proof after deadline rejected");

    expect_error([&server_token, now] {
        static_cast<void>(lanlink::auth::ServerHandshake(
            server_token, 0, now + std::chrono::seconds{1}));
    }, "zero connection binding rejected");

    lanlink::auth::ClientHandshake zero_request(identity, client_token);
    expect_error([&zero_request] {
        static_cast<void>(zero_request.begin(0));
    }, "zero authentication request id rejected");
}

void test_stream_decoder() {
    auto token = lanlink::auth::AuthToken::from_secret(
        "dddddddddddddddddddddddddddddddd");
    const auto proof = token.proof({});
    lanlink::auth::ClientProofPayload payload;
    payload.token_proof = proof;

    lanlink::protocol::Frame frame;
    frame.type = lanlink::protocol::MessageType::client_auth;
    frame.request_id = 5;
    frame.payload = lanlink::auth::encode_client_proof(payload);
    const auto encoded = lanlink::protocol::encode_frame(frame);
    lanlink::protocol::FrameStreamDecoder decoder;
    std::vector<lanlink::protocol::Frame> decoded;

    for (const auto byte : encoded) {
        const auto frames = decoder.push(std::span<const std::byte>(&byte, 1));
        decoded.insert(decoded.end(), frames.begin(), frames.end());
    }

    expect(decoded.size() == 1, "fragmented auth frame count");
    expect(!decoded.empty() && decoded.front() == frame, "fragmented auth frame value");
    expect(decoder.buffered_size() == 0, "fragmented auth frame drained");
}

}

int main() {
    const auto directory = make_test_directory();

    try {
        test_identity(directory);
#ifdef _WIN32
        test_legacy_identity_migration(directory);
#endif
        test_token(directory);
        test_payloads();
        test_successful_handshake(directory);
        test_rejected_handshake(directory);
        test_strict_state_timeout_and_replay(directory);
        test_stream_decoder();
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
        std::cerr << failures << " test failure(s)\n";
        return 1;
    }

    std::cout << "all auth tests passed\n";
    return 0;
}
