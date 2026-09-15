#include "lanlink/auth/handshake.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace lanlink::auth {
namespace {

constexpr std::size_t client_hello_size = public_key_size + nonce_size;
constexpr std::size_t server_challenge_size = nonce_size;
constexpr std::size_t client_proof_size = signature_size + token_proof_size;
constexpr std::size_t auth_result_size = 1;

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
    append(output, payload.client_nonce);
    return output;
}

ClientHelloPayload decode_client_hello(const std::vector<std::byte>& payload) {
    if (payload.size() != client_hello_size) {
        throw std::runtime_error("invalid client hello payload");
    }

    return {
        read_array<public_key_size>(payload, 0),
        read_array<nonce_size>(payload, public_key_size),
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
    return {payload.accepted ? std::byte{1} : std::byte{0}};
}

AuthResultPayload decode_auth_result(const std::vector<std::byte>& payload) {
    if (payload.size() != auth_result_size ||
        (payload.front() != std::byte{0} && payload.front() != std::byte{1})) {
        throw std::runtime_error("invalid authentication result payload");
    }

    return {payload.front() == std::byte{1}};
}

ClientHandshake::ClientHandshake(const DeviceIdentity& identity, const AuthToken& token) noexcept
    : identity_(identity), token_(token) {
}

protocol::Frame ClientHandshake::begin(const std::uint32_t request_id) {
    if (state_ != State::ready) {
        throw std::logic_error("client authentication already started");
    }

    request_id_ = request_id;
    client_nonce_ = random_nonce();
    state_ = State::waiting_challenge;

    protocol::Frame frame;
    frame.type = protocol::MessageType::client_hello;
    frame.request_id = request_id_;
    frame.payload = encode_client_hello({identity_.public_key(), client_nonce_});
    return frame;
}

protocol::Frame ClientHandshake::handle_challenge(const protocol::Frame& frame) {
    if (state_ != State::waiting_challenge) {
        throw std::logic_error("client is not waiting for an authentication challenge");
    }

    require_frame(frame, protocol::MessageType::server_hello, request_id_);
    const auto challenge = decode_server_challenge(frame.payload);
    const auto transcript =
        make_auth_transcript(identity_.public_key(), client_nonce_, challenge.server_nonce);
    ClientProofPayload proof;
    proof.signature = identity_.sign(transcript);
    proof.token_proof = token_.proof(transcript);

    protocol::Frame response;
    response.type = protocol::MessageType::client_auth;
    response.request_id = request_id_;
    response.payload = encode_client_proof(proof);
    state_ = State::waiting_result;
    return response;
}

bool ClientHandshake::handle_result(const protocol::Frame& frame) {
    if (state_ != State::waiting_result) {
        throw std::logic_error("client is not waiting for an authentication result");
    }

    require_frame(frame, protocol::MessageType::auth_result, request_id_);
    const auto result = decode_auth_result(frame.payload);
    state_ = result.accepted ? State::authenticated : State::rejected;
    return result.accepted;
}

bool ClientHandshake::authenticated() const noexcept {
    return state_ == State::authenticated;
}

ServerHandshake::ServerHandshake(const AuthToken& token) noexcept : token_(token) {
}

protocol::Frame ServerHandshake::handle_hello(const protocol::Frame& frame) {
    if (state_ != State::waiting_hello || frame.type != protocol::MessageType::client_hello) {
        throw std::runtime_error("unexpected client authentication hello");
    }

    const auto hello = decode_client_hello(frame.payload);
    public_key_ = hello.public_key;
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

protocol::Frame ServerHandshake::handle_proof(const protocol::Frame& frame) {
    if (state_ != State::waiting_proof) {
        throw std::runtime_error("unexpected client authentication proof");
    }

    require_frame(frame, protocol::MessageType::client_auth, request_id_);
    const auto proof = decode_client_proof(frame.payload);
    const auto transcript = make_auth_transcript(public_key_, client_nonce_, server_nonce_);
    const auto valid_signature = verify_signature(public_key_, transcript, proof.signature);
    const auto valid_token = token_.verify(transcript, proof.token_proof);
    const auto accepted = valid_signature && valid_token;
    state_ = accepted ? State::authenticated : State::rejected;

    protocol::Frame response;
    response.type = protocol::MessageType::auth_result;
    response.request_id = request_id_;
    response.payload = encode_auth_result({accepted});
    return response;
}

bool ServerHandshake::authenticated() const noexcept {
    return state_ == State::authenticated;
}

bool ServerHandshake::rejected() const noexcept {
    return state_ == State::rejected;
}

std::string ServerHandshake::device_id_hex() const {
    if (!authenticated()) {
        throw std::logic_error("device is not authenticated");
    }

    const auto id = make_device_id(public_key_);
    return hex_encode(id);
}

}
