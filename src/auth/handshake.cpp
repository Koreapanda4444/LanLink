#include "lanlink/auth/handshake.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <utility>

namespace lanlink::auth {
namespace {

constexpr std::size_t client_hello_size = public_key_size +
                                          encryption_public_key_size + nonce_size;
constexpr std::size_t server_challenge_size = nonce_size;
constexpr std::size_t client_proof_size = signature_size + token_proof_size;
constexpr std::size_t auth_result_size = 1 + session_id_size;

template <std::size_t Size>
void append(std::vector<std::byte>& destination, const std::array<std::byte, Size>& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

template <std::size_t Size>
std::array<std::byte, Size> read_array(const std::vector<std::byte>& source,
                                       const std::size_t offset) {
    std::array<std::byte, Size> result{};
    std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(offset), Size, result.begin());
    return result;
}

template <std::size_t Size>
bool all_zero(const std::array<std::byte, Size>& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
        return byte == std::byte{0};
    });
}

template <std::size_t Size>
void cleanse(std::array<std::byte, Size>& value) noexcept {
    OPENSSL_cleanse(value.data(), value.size());
}

void cleanse(std::vector<std::byte>& value) noexcept {
    if (!value.empty()) {
        OPENSSL_cleanse(value.data(), value.size());
    }
}

SessionId make_nonzero_session_id() {
    SessionId result{};

    do {
        result = random_session_id();
    } while (all_zero(result));

    return result;
}

void require_frame(const protocol::Frame& frame,
                   const protocol::MessageType type,
                   const std::uint32_t request_id) {
    if (frame.type != type || frame.request_id != request_id) {
        throw std::runtime_error("unexpected authentication frame");
    }
}

}

std::vector<std::byte> encode_client_hello(const ClientHelloPayload& payload) {
    std::vector<std::byte> output;
    output.reserve(client_hello_size);
    append(output, payload.public_key);
    append(output, payload.encryption_public_key);
    append(output, payload.client_nonce);
    return output;
}

ClientHelloPayload decode_client_hello(const std::vector<std::byte>& payload) {
    if (payload.size() != client_hello_size) {
        throw std::runtime_error("invalid client hello payload");
    }

    return {
        read_array<public_key_size>(payload, 0),
        read_array<encryption_public_key_size>(payload, public_key_size),
        read_array<nonce_size>(payload, public_key_size + encryption_public_key_size),
    };
}

std::vector<std::byte> encode_server_challenge(const ServerChallengePayload& payload) {
    return {payload.server_nonce.begin(), payload.server_nonce.end()};
}

ServerChallengePayload decode_server_challenge(const std::vector<std::byte>& payload) {
    if (payload.size() != server_challenge_size) {
        throw std::runtime_error("invalid server challenge payload");
    }

    return {read_array<nonce_size>(payload, 0)};
}

std::vector<std::byte> encode_client_proof(const ClientProofPayload& payload) {
    std::vector<std::byte> output;
    output.reserve(client_proof_size);
    append(output, payload.signature);
    append(output, payload.token_proof);
    return output;
}

ClientProofPayload decode_client_proof(const std::vector<std::byte>& payload) {
    if (payload.size() != client_proof_size) {
        throw std::runtime_error("invalid client proof payload");
    }

    return {
        read_array<signature_size>(payload, 0),
        read_array<token_proof_size>(payload, signature_size),
    };
}

std::vector<std::byte> encode_auth_result(const AuthResultPayload& payload) {
    const auto empty_session = all_zero(payload.session_id);

    if ((payload.accepted && empty_session) || (!payload.accepted && !empty_session)) {
        throw std::invalid_argument("authentication result has invalid session state");
    }

    std::vector<std::byte> output;
    output.reserve(auth_result_size);
    output.push_back(payload.accepted ? std::byte{1} : std::byte{0});
    append(output, payload.session_id);
    return output;
}

AuthResultPayload decode_auth_result(const std::vector<std::byte>& payload) {
    if (payload.size() != auth_result_size ||
        (payload.front() != std::byte{0} && payload.front() != std::byte{1})) {
        throw std::runtime_error("invalid authentication result payload");
    }

    AuthResultPayload result;
    result.accepted = payload.front() == std::byte{1};
    result.session_id = read_array<session_id_size>(payload, 1);
    const auto empty_session = all_zero(result.session_id);

    if ((result.accepted && empty_session) || (!result.accepted && !empty_session)) {
        throw std::runtime_error("authentication result has invalid session state");
    }

    return result;
}

ClientHandshake::ClientHandshake(const DeviceIdentity& identity, const AuthToken& token)
    : identity_(identity), token_(token), encryption_key_(DeviceEncryptionKey::generate()) {
}

ClientHandshake::~ClientHandshake() {
    close();
}

protocol::Frame ClientHandshake::begin(const std::uint32_t request_id) {
    if (state_ != State::ready) {
        throw std::logic_error("client authentication already started");
    }

    if (request_id == 0) {
        throw std::invalid_argument("authentication request id must be non-zero");
    }

    request_id_ = request_id;
    client_nonce_ = random_nonce();
    state_ = State::waiting_challenge;

    protocol::Frame frame;
    frame.type = protocol::MessageType::client_hello;
    frame.request_id = request_id_;
    frame.payload = encode_client_hello({identity_.public_key(),
                                         encryption_key_.public_key(), client_nonce_});
    return frame;
}

protocol::Frame ClientHandshake::handle_challenge(const protocol::Frame& frame) {
    if (state_ != State::waiting_challenge) {
        throw std::logic_error("client is not waiting for an authentication challenge");
    }

    require_frame(frame, protocol::MessageType::server_hello, request_id_);
    const auto challenge = decode_server_challenge(frame.payload);
    auto transcript = make_auth_transcript(identity_.public_key(),
                                           encryption_key_.public_key(),
                                           client_nonce_, challenge.server_nonce);
    ClientProofPayload proof;

    try {
        proof.signature = identity_.sign(transcript);
        proof.token_proof = token_.proof(transcript);
        signed_device_key_ = SignedDeviceKey{identity_.public_key(),
                                             encryption_key_.public_key(),
                                             client_nonce_, challenge.server_nonce,
                                             proof.signature};

        protocol::Frame response;
        response.type = protocol::MessageType::client_auth;
        response.request_id = request_id_;
        response.payload = encode_client_proof(proof);
        state_ = State::waiting_result;
        cleanse(transcript);
        cleanse(proof.signature);
        cleanse(proof.token_proof);
        return response;
    } catch (...) {
        signed_device_key_.reset();
        cleanse(transcript);
        cleanse(proof.signature);
        cleanse(proof.token_proof);
        throw;
    }
}

bool ClientHandshake::handle_result(const protocol::Frame& frame) {
    if (state_ != State::waiting_result) {
        throw std::logic_error("client is not waiting for an authentication result");
    }

    require_frame(frame, protocol::MessageType::auth_result, request_id_);
    const auto result = decode_auth_result(frame.payload);
    cleanse(client_nonce_);

    if (result.accepted) {
        session_id_ = result.session_id;
        state_ = State::authenticated;
    } else {
        cleanse(session_id_);
        signed_device_key_.reset();
        state_ = State::rejected;
    }

    return result.accepted;
}

bool ClientHandshake::authenticated() const noexcept {
    return state_ == State::authenticated;
}

std::optional<SessionId> ClientHandshake::session_id() const noexcept {
    if (!authenticated()) {
        return std::nullopt;
    }

    return session_id_;
}

std::optional<SignedDeviceKey> ClientHandshake::signed_device_key() const noexcept {
    return authenticated() ? signed_device_key_ : std::nullopt;
}

const DeviceEncryptionKey& ClientHandshake::encryption_key() const {
    if (!authenticated()) {
        throw std::logic_error("client encryption key is not authenticated");
    }
    return encryption_key_;
}

void ClientHandshake::close() noexcept {
    signed_device_key_.reset();
    cleanse(client_nonce_);
    cleanse(session_id_);
    request_id_ = 0;
    state_ = State::closed;
}

ServerHandshake::ServerHandshake(const AuthToken& token,
                                 const std::uintptr_t connection_binding,
                                 const Clock::time_point deadline)
    : token_(token), connection_binding_(connection_binding), deadline_(deadline) {
    if (connection_binding_ == 0) {
        throw std::invalid_argument("authentication connection binding must be non-zero");
    }
}

ServerHandshake::~ServerHandshake() {
    close();
}

void ServerHandshake::require_active(const Clock::time_point now) {
    if (expire(now)) {
        throw std::runtime_error("authentication deadline expired");
    }

    if (state_ == State::closed) {
        throw std::logic_error("authentication session is closed");
    }
}

protocol::Frame ServerHandshake::handle_hello(const protocol::Frame& frame,
                                              const Clock::time_point now) {
    require_active(now);

    if (state_ != State::waiting_hello || frame.type != protocol::MessageType::client_hello) {
        throw std::runtime_error("unexpected client authentication hello");
    }

    if (frame.request_id == 0) {
        throw std::runtime_error("authentication request id must be non-zero");
    }

    const auto hello = decode_client_hello(frame.payload);
    if (all_zero(hello.encryption_public_key)) {
        throw std::runtime_error("client encryption public key is empty");
    }
    public_key_ = hello.public_key;
    encryption_public_key_ = hello.encryption_public_key;
    client_nonce_ = hello.client_nonce;
    server_nonce_ = random_nonce();
    request_id_ = frame.request_id;
    state_ = State::waiting_proof;

    protocol::Frame response;
    response.type = protocol::MessageType::server_hello;
    response.request_id = request_id_;
    response.payload = encode_server_challenge({server_nonce_});
    return response;
}

protocol::Frame ServerHandshake::handle_proof(const protocol::Frame& frame,
                                              const Clock::time_point now) {
    require_active(now);

    if (state_ != State::waiting_proof) {
        throw std::runtime_error("unexpected client authentication proof");
    }

    require_frame(frame, protocol::MessageType::client_auth, request_id_);
    auto proof = decode_client_proof(frame.payload);
    auto transcript = make_auth_transcript(public_key_, encryption_public_key_,
                                          client_nonce_, server_nonce_);
    bool accepted = false;

    try {
        const auto valid_signature = verify_signature(public_key_, transcript, proof.signature);
        const auto valid_token = token_.verify(transcript, proof.token_proof);
        accepted = valid_signature && valid_token;

        if (accepted) {
            device_id_ = make_device_id(public_key_);
            session_id_ = make_nonzero_session_id();
            signed_device_key_ = SignedDeviceKey{public_key_, encryption_public_key_,
                                                 client_nonce_, server_nonce_, proof.signature};
            state_ = State::authenticated;
        } else {
            signed_device_key_.reset();
            cleanse(device_id_);
            cleanse(session_id_);
            state_ = State::rejected;
        }

        protocol::Frame response;
        response.type = protocol::MessageType::auth_result;
        response.request_id = request_id_;
        response.payload = encode_auth_result({accepted, session_id_});
        cleanse(transcript);
        cleanse(proof.signature);
        cleanse(proof.token_proof);
        cleanse(public_key_);
        cleanse(encryption_public_key_);
        cleanse(client_nonce_);
        cleanse(server_nonce_);
        return response;
    } catch (...) {
        cleanse(transcript);
        cleanse(proof.signature);
        cleanse(proof.token_proof);
        cleanse(public_key_);
        cleanse(encryption_public_key_);
        cleanse(client_nonce_);
        cleanse(server_nonce_);
        cleanse(device_id_);
        cleanse(session_id_);
        state_ = State::rejected;
        throw;
    }
}

bool ServerHandshake::authenticated() const noexcept {
    return state_ == State::authenticated;
}

bool ServerHandshake::rejected() const noexcept {
    return state_ == State::rejected;
}

bool ServerHandshake::timed_out() const noexcept {
    return state_ == State::timed_out;
}

bool ServerHandshake::expire(const Clock::time_point now) noexcept {
    if ((state_ == State::waiting_hello || state_ == State::waiting_proof) && now >= deadline_) {
        cleanse(public_key_);
        cleanse(encryption_public_key_);
        cleanse(client_nonce_);
        cleanse(server_nonce_);
        request_id_ = 0;
        state_ = State::timed_out;
        return true;
    }

    return state_ == State::timed_out;
}

SessionId ServerHandshake::session_id() const {
    if (!authenticated()) {
        throw std::logic_error("session is not authenticated");
    }

    return session_id_;
}

DeviceId ServerHandshake::device_id() const {
    if (!authenticated()) {
        throw std::logic_error("device is not authenticated");
    }

    return device_id_;
}

SignedDeviceKey ServerHandshake::signed_device_key() const {
    if (!authenticated() || !signed_device_key_) {
        throw std::logic_error("device encryption key is not authenticated");
    }
    return *signed_device_key_;
}

std::string ServerHandshake::device_id_hex() const {
    const auto id = device_id();
    return hex_encode(id);
}

bool ServerHandshake::bound_to(const std::uintptr_t connection_binding,
                               const DeviceId& device_id_value,
                               const SessionId& session_id_value) const noexcept {
    return authenticated() && connection_binding_ == connection_binding &&
           device_id_ == device_id_value && session_id_ == session_id_value;
}

void ServerHandshake::close() noexcept {
    signed_device_key_.reset();
    cleanse(public_key_);
    cleanse(encryption_public_key_);
    cleanse(client_nonce_);
    cleanse(server_nonce_);
    cleanse(device_id_);
    cleanse(session_id_);
    connection_binding_ = 0;
    request_id_ = 0;
    state_ = State::closed;
}

}
