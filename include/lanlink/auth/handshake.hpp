#pragma once

#include "lanlink/auth/identity.hpp"
#include "lanlink/protocol/message.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lanlink::auth {

struct ClientHelloPayload {
    PublicKey public_key{};
    Nonce client_nonce{};

    bool operator==(const ClientHelloPayload&) const = default;
};

struct ServerChallengePayload {
    Nonce server_nonce{};

    bool operator==(const ServerChallengePayload&) const = default;
};

struct ClientProofPayload {
    Signature signature{};
    TokenProof token_proof{};

    bool operator==(const ClientProofPayload&) const = default;
};

struct AuthResultPayload {
    bool accepted = false;
    SessionId session_id{};

    bool operator==(const AuthResultPayload&) const = default;
};

[[nodiscard]] std::vector<std::byte> encode_client_hello(const ClientHelloPayload& payload);
[[nodiscard]] ClientHelloPayload decode_client_hello(const std::vector<std::byte>& payload);
[[nodiscard]] std::vector<std::byte> encode_server_challenge(
    const ServerChallengePayload& payload);
[[nodiscard]] ServerChallengePayload decode_server_challenge(
    const std::vector<std::byte>& payload);
[[nodiscard]] std::vector<std::byte> encode_client_proof(const ClientProofPayload& payload);
[[nodiscard]] ClientProofPayload decode_client_proof(const std::vector<std::byte>& payload);
[[nodiscard]] std::vector<std::byte> encode_auth_result(const AuthResultPayload& payload);
[[nodiscard]] AuthResultPayload decode_auth_result(const std::vector<std::byte>& payload);

class ClientHandshake {
public:
    ClientHandshake(const DeviceIdentity& identity, const AuthToken& token) noexcept;
    ~ClientHandshake();

    ClientHandshake(const ClientHandshake&) = delete;
    ClientHandshake& operator=(const ClientHandshake&) = delete;
    ClientHandshake(ClientHandshake&&) = delete;
    ClientHandshake& operator=(ClientHandshake&&) = delete;

    [[nodiscard]] protocol::Frame begin(std::uint32_t request_id);
    [[nodiscard]] protocol::Frame handle_challenge(const protocol::Frame& frame);
    [[nodiscard]] bool handle_result(const protocol::Frame& frame);
    [[nodiscard]] bool authenticated() const noexcept;
    [[nodiscard]] std::optional<SessionId> session_id() const noexcept;
    void close() noexcept;

private:
    enum class State {
        ready,
        waiting_challenge,
        waiting_result,
        authenticated,
        rejected,
        closed,
    };

    const DeviceIdentity& identity_;
    const AuthToken& token_;
    Nonce client_nonce_{};
    SessionId session_id_{};
    std::uint32_t request_id_ = 0;
    State state_ = State::ready;
};

class ServerHandshake {
public:
    using Clock = std::chrono::steady_clock;

    ServerHandshake(const AuthToken& token,
                    std::uintptr_t connection_binding,
                    Clock::time_point deadline);
    ~ServerHandshake();

    ServerHandshake(const ServerHandshake&) = delete;
    ServerHandshake& operator=(const ServerHandshake&) = delete;
    ServerHandshake(ServerHandshake&&) = delete;
    ServerHandshake& operator=(ServerHandshake&&) = delete;

    [[nodiscard]] protocol::Frame handle_hello(
        const protocol::Frame& frame,
        Clock::time_point now = Clock::now());
    [[nodiscard]] protocol::Frame handle_proof(
        const protocol::Frame& frame,
        Clock::time_point now = Clock::now());
    [[nodiscard]] bool authenticated() const noexcept;
    [[nodiscard]] bool rejected() const noexcept;
    [[nodiscard]] bool timed_out() const noexcept;
    [[nodiscard]] bool expire(Clock::time_point now = Clock::now()) noexcept;
    [[nodiscard]] SessionId session_id() const;
    [[nodiscard]] DeviceId device_id() const;
    [[nodiscard]] std::string device_id_hex() const;
    [[nodiscard]] bool bound_to(std::uintptr_t connection_binding,
                                const DeviceId& device_id,
                                const SessionId& session_id) const noexcept;
    void close() noexcept;

private:
    enum class State {
        waiting_hello,
        waiting_proof,
        authenticated,
        rejected,
        timed_out,
        closed,
    };

    void require_active(Clock::time_point now);

    const AuthToken& token_;
    std::uintptr_t connection_binding_ = 0;
    Clock::time_point deadline_{};
    PublicKey public_key_{};
    Nonce client_nonce_{};
    Nonce server_nonce_{};
    DeviceId device_id_{};
    SessionId session_id_{};
    std::uint32_t request_id_ = 0;
    State state_ = State::waiting_hello;
};

}
