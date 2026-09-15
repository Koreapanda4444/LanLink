#pragma once

#include "lanlink/auth/identity.hpp"
#include "lanlink/protocol/message.hpp"

#include <cstdint>
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

    [[nodiscard]] protocol::Frame begin(std::uint32_t request_id);
    [[nodiscard]] protocol::Frame handle_challenge(const protocol::Frame& frame);
    [[nodiscard]] bool handle_result(const protocol::Frame& frame);
    [[nodiscard]] bool authenticated() const noexcept;

private:
    enum class State {
        ready,
        waiting_challenge,
        waiting_result,
        authenticated,
        rejected,
    };

    const DeviceIdentity& identity_;
    const AuthToken& token_;
    Nonce client_nonce_{};
    std::uint32_t request_id_ = 0;
    State state_ = State::ready;
};

class ServerHandshake {
public:
    explicit ServerHandshake(const AuthToken& token) noexcept;

    [[nodiscard]] protocol::Frame handle_hello(const protocol::Frame& frame);
    [[nodiscard]] protocol::Frame handle_proof(const protocol::Frame& frame);
    [[nodiscard]] bool authenticated() const noexcept;
    [[nodiscard]] bool rejected() const noexcept;
    [[nodiscard]] std::string device_id_hex() const;

private:
    enum class State {
        waiting_hello,
        waiting_proof,
        authenticated,
        rejected,
    };

    const AuthToken& token_;
    PublicKey public_key_{};
    Nonce client_nonce_{};
    Nonce server_nonce_{};
    std::uint32_t request_id_ = 0;
    State state_ = State::waiting_hello;
};

}
