#include "lanlink/protocol/network_messages.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace lanlink::protocol {
namespace {

constexpr std::size_t network_selection_size = network_id_size;
constexpr std::size_t network_member_size = network_id_size + network_device_id_size;
constexpr std::size_t network_operation_result_size = 1 + 2 + network_id_size;
constexpr std::size_t network_event_size = 1 + network_id_size + network_device_id_size;
constexpr std::uint32_t peer_pool_first = 0x0a400000U;
constexpr std::uint32_t peer_pool_end = 0x0a800000U;
constexpr std::uint32_t peer_subnet_size = 256;

template <typename Integer>
void append_integer(std::vector<std::byte>& output, const Integer value) {
    static_assert(std::is_unsigned_v<Integer>);

    for (std::size_t index = sizeof(Integer); index > 0; --index) {
        const auto shift = static_cast<unsigned>((index - 1) * 8);
        output.push_back(std::byte{static_cast<unsigned char>((value >> shift) & 0xffU)});
    }
}

template <std::size_t Size>
void append_array(std::vector<std::byte>& output,
                  const std::array<std::byte, Size>& value) {
    output.insert(output.end(), value.begin(), value.end());
}

class Reader {
public:
    explicit Reader(const std::span<const std::byte> payload) : payload_(payload) {
    }

    template <typename Integer>
    [[nodiscard]] Integer read_integer() {
        static_assert(std::is_unsigned_v<Integer>);
        require(sizeof(Integer));
        Integer result = 0;

        for (std::size_t index = 0; index < sizeof(Integer); ++index) {
            result = static_cast<Integer>(
                (result << 8U) |
                std::to_integer<unsigned char>(payload_[offset_ + index]));
        }

        offset_ += sizeof(Integer);
        return result;
    }

    template <std::size_t Size>
    [[nodiscard]] std::array<std::byte, Size> read_array() {
        require(Size);
        std::array<std::byte, Size> result{};
        std::copy_n(payload_.begin() + static_cast<std::ptrdiff_t>(offset_),
                    Size,
                    result.begin());
        offset_ += Size;
        return result;
    }

    [[nodiscard]] std::string read_string(const std::size_t size) {
        require(size);

        if (size == 0) {
            return {};
        }

        const auto* data = reinterpret_cast<const char*>(payload_.data() + offset_);
        std::string result(data, size);
        offset_ += size;
        return result;
    }

    void require_finished() const {
        if (offset_ != payload_.size()) {
            throw std::runtime_error("network payload contains trailing bytes");
        }
    }

private:
    void require(const std::size_t size) const {
        if (size > payload_.size() - offset_) {
            throw std::runtime_error("network payload is truncated");
        }
    }

    std::span<const std::byte> payload_;
    std::size_t offset_ = 0;
};

template <std::size_t Size>
bool is_zero(const std::array<std::byte, Size>& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
        return byte == std::byte{0};
    });
}

bool valid_network_name(const std::string_view name) noexcept {
    if (name.empty() || name.size() > max_network_name_size) {
        return false;
    }

    std::size_t offset = 0;

    while (offset < name.size()) {
        const auto first = static_cast<unsigned char>(name[offset]);
        std::uint32_t code_point = 0;
        std::size_t continuation_count = 0;

        if (first <= 0x7fU) {
            code_point = first;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            code_point = first & 0x1fU;
            continuation_count = 1;
        } else if (first >= 0xe0U && first <= 0xefU) {
            code_point = first & 0x0fU;
            continuation_count = 2;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            code_point = first & 0x07U;
            continuation_count = 3;
        } else {
            return false;
        }

        if (continuation_count > name.size() - offset - 1) {
            return false;
        }

        for (std::size_t index = 1; index <= continuation_count; ++index) {
            const auto next = static_cast<unsigned char>(name[offset + index]);

            if ((next & 0xc0U) != 0x80U) {
                return false;
            }

            code_point = (code_point << 6U) | (next & 0x3fU);
        }

        const bool overlong =
            (continuation_count == 1 && code_point < 0x80U) ||
            (continuation_count == 2 && code_point < 0x800U) ||
            (continuation_count == 3 && code_point < 0x10000U);
        const bool surrogate = code_point >= 0xd800U && code_point <= 0xdfffU;
        const bool control = code_point < 0x20U ||
                             (code_point >= 0x7fU && code_point <= 0x9fU);

        if (overlong || surrogate || code_point > 0x10ffffU || control) {
            return false;
        }

        offset += continuation_count + 1;
    }

    return true;
}

void require_network_id_for_encode(const NetworkId& network_id) {
    if (is_zero(network_id)) {
        throw std::invalid_argument("network id must not be zero");
    }
}

void require_device_id_for_encode(const NetworkDeviceId& device_id) {
    if (is_zero(device_id)) {
        throw std::invalid_argument("network device id must not be zero");
    }
}

void require_network_id_for_decode(const NetworkId& network_id) {
    if (is_zero(network_id)) {
        throw std::runtime_error("network payload contains a zero network id");
    }
}

void require_device_id_for_decode(const NetworkDeviceId& device_id) {
    if (is_zero(device_id)) {
        throw std::runtime_error("network payload contains a zero device id");
    }
}

void validate_result_for_encode(const NetworkOperationResult& result) {
    if (!is_known_network_operation(result.operation)) {
        throw std::invalid_argument("unknown network operation");
    }
    if (!is_known_network_result_code(result.code)) {
        throw std::invalid_argument("unknown network result code");
    }
    if (result.code == NetworkResultCode::pending_approval &&
        result.operation != NetworkOperation::join) {
        throw std::invalid_argument("pending approval is only valid for join operations");
    }

    const bool zero_id = is_zero(result.network_id);

    if (result.operation == NetworkOperation::list) {
        if (!zero_id) {
            throw std::invalid_argument("list result must not contain a network id");
        }
        return;
    }

    if (result.operation == NetworkOperation::create &&
        result.code != NetworkResultCode::success) {
        if (!zero_id) {
            throw std::invalid_argument("failed create result must not contain a network id");
        }
        return;
    }

    require_network_id_for_encode(result.network_id);
}

void validate_result_for_decode(const NetworkOperationResult& result) {
    if (!is_known_network_operation(result.operation) ||
        !is_known_network_result_code(result.code)) {
        throw std::runtime_error("network operation result contains an unknown value");
    }
    if (result.code == NetworkResultCode::pending_approval &&
        result.operation != NetworkOperation::join) {
        throw std::runtime_error("network operation result has invalid pending state");
    }

    const bool zero_id = is_zero(result.network_id);

    if (result.operation == NetworkOperation::list) {
        if (!zero_id) {
            throw std::runtime_error("network list result contains an unexpected id");
        }
        return;
    }

    if (result.operation == NetworkOperation::create &&
        result.code != NetworkResultCode::success) {
        if (!zero_id) {
            throw std::runtime_error("failed network create result contains an id");
        }
        return;
    }

    require_network_id_for_decode(result.network_id);
}

void validate_summary_for_encode(const NetworkSummary& summary) {
    require_network_id_for_encode(summary.network_id);
    require_device_id_for_encode(summary.owner_device_id);

    if (summary.role != NetworkRole::member && summary.role != NetworkRole::owner) {
        throw std::invalid_argument("unknown network role");
    }
    if (summary.created_at_ms < 0) {
        throw std::invalid_argument("network creation timestamp must not be negative");
    }
    if (summary.member_count == 0) {
        throw std::invalid_argument("network member count must be positive");
    }
    if (!valid_network_name(summary.name)) {
        throw std::invalid_argument("network name is invalid");
    }
}

void validate_summary_for_decode(const NetworkSummary& summary) {
    require_network_id_for_decode(summary.network_id);
    require_device_id_for_decode(summary.owner_device_id);

    if (summary.role != NetworkRole::member && summary.role != NetworkRole::owner) {
        throw std::runtime_error("network summary contains an unknown role");
    }
    if (summary.created_at_ms < 0 || summary.member_count == 0 ||
        !valid_network_name(summary.name)) {
        throw std::runtime_error("network summary contains invalid fields");
    }
}

template <typename Collection>
bool contains_network_id(const Collection& values, const NetworkId& network_id) {
    return std::any_of(values.begin(), values.end(), [&](const auto& value) {
        return value.network_id == network_id;
    });
}

void append_summary(std::vector<std::byte>& output, const NetworkSummary& summary) {
    validate_summary_for_encode(summary);
    append_array(output, summary.network_id);
    append_array(output, summary.owner_device_id);
    append_integer(output, static_cast<std::uint8_t>(summary.role));
    append_integer(output, static_cast<std::uint64_t>(summary.created_at_ms));
    append_integer(output, summary.member_count);
    append_integer(output, static_cast<std::uint16_t>(summary.name.size()));
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(summary.name.data()),
                  reinterpret_cast<const std::byte*>(summary.name.data() + summary.name.size()));
}

NetworkSummary read_summary(Reader& reader) {
    NetworkSummary summary;
    summary.network_id = reader.read_array<network_id_size>();
    summary.owner_device_id = reader.read_array<network_device_id_size>();
    summary.role = static_cast<NetworkRole>(reader.read_integer<std::uint8_t>());
    const auto timestamp = reader.read_integer<std::uint64_t>();

    if (timestamp > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("network creation timestamp is out of range");
    }

    summary.created_at_ms = static_cast<std::int64_t>(timestamp);
    summary.member_count = reader.read_integer<std::uint32_t>();
    const auto name_size = reader.read_integer<std::uint16_t>();
    summary.name = reader.read_string(name_size);
    validate_summary_for_decode(summary);
    return summary;
}

bool valid_peer_state(const NetworkPeerState& state) {
    if (is_zero(state.network_id) || state.prefix_length != 24 ||
        state.subnet_address < peer_pool_first ||
        state.subnet_address >= peer_pool_end ||
        state.subnet_address % peer_subnet_size != 0 ||
        state.own_address <= state.subnet_address ||
        state.own_address >= state.subnet_address + peer_subnet_size - 1 ||
        state.peers.size() > max_network_peer_entries) {
        return false;
    }

    std::set<NetworkDeviceId> device_ids;
    std::set<std::uint32_t> addresses{state.own_address};
    for (const auto& peer : state.peers) {
        if (is_zero(peer.device_id) ||
            peer.ipv4_address <= state.subnet_address ||
            peer.ipv4_address >= state.subnet_address + peer_subnet_size - 1 ||
            !device_ids.insert(peer.device_id).second ||
            !addresses.insert(peer.ipv4_address).second) {
            return false;
        }
    }
    return true;
}

}

std::vector<std::byte> encode_network_create_request(const NetworkCreateRequest& request) {
    if (!valid_network_name(request.name)) {
        throw std::invalid_argument("network name is invalid");
    }

    std::vector<std::byte> output;
    output.reserve(2 + request.name.size());
    append_integer(output, static_cast<std::uint16_t>(request.name.size()));
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(request.name.data()),
                  reinterpret_cast<const std::byte*>(request.name.data() + request.name.size()));
    return output;
}

NetworkCreateRequest decode_network_create_request(const std::span<const std::byte> payload) {
    Reader reader(payload);
    const auto name_size = reader.read_integer<std::uint16_t>();
    NetworkCreateRequest request{reader.read_string(name_size)};
    reader.require_finished();

    if (!valid_network_name(request.name)) {
        throw std::runtime_error("network create request contains an invalid name");
    }

    return request;
}

std::vector<std::byte> encode_network_list_request(const NetworkListRequest&) {
    return {};
}

NetworkListRequest decode_network_list_request(const std::span<const std::byte> payload) {
    if (!payload.empty()) {
        throw std::runtime_error("network list request must be empty");
    }

    return {};
}

std::vector<std::byte> encode_network_selection_request(
    const NetworkSelectionRequest& request) {
    require_network_id_for_encode(request.network_id);
    return {request.network_id.begin(), request.network_id.end()};
}

NetworkSelectionRequest decode_network_selection_request(
    const std::span<const std::byte> payload) {
    if (payload.size() != network_selection_size) {
        throw std::runtime_error("network selection request has an invalid size");
    }

    Reader reader(payload);
    NetworkSelectionRequest request{reader.read_array<network_id_size>()};
    reader.require_finished();
    require_network_id_for_decode(request.network_id);
    return request;
}

std::vector<std::byte> encode_network_member_request(const NetworkMemberRequest& request) {
    require_network_id_for_encode(request.network_id);
    require_device_id_for_encode(request.device_id);
    std::vector<std::byte> output;
    output.reserve(network_member_size);
    append_array(output, request.network_id);
    append_array(output, request.device_id);
    return output;
}

NetworkMemberRequest decode_network_member_request(const std::span<const std::byte> payload) {
    if (payload.size() != network_member_size) {
        throw std::runtime_error("network member request has an invalid size");
    }

    Reader reader(payload);
    NetworkMemberRequest request;
    request.network_id = reader.read_array<network_id_size>();
    request.device_id = reader.read_array<network_device_id_size>();
    reader.require_finished();
    require_network_id_for_decode(request.network_id);
    require_device_id_for_decode(request.device_id);
    return request;
}

std::vector<std::byte> encode_network_operation_result(
    const NetworkOperationResult& result) {
    validate_result_for_encode(result);
    std::vector<std::byte> output;
    output.reserve(network_operation_result_size);
    append_integer(output, static_cast<std::uint8_t>(result.operation));
    append_integer(output, static_cast<std::uint16_t>(result.code));
    append_array(output, result.network_id);
    return output;
}

NetworkOperationResult decode_network_operation_result(
    const std::span<const std::byte> payload) {
    if (payload.size() != network_operation_result_size) {
        throw std::runtime_error("network operation result has an invalid size");
    }

    Reader reader(payload);
    NetworkOperationResult result;
    result.operation = static_cast<NetworkOperation>(reader.read_integer<std::uint8_t>());
    result.code = static_cast<NetworkResultCode>(reader.read_integer<std::uint16_t>());
    result.network_id = reader.read_array<network_id_size>();
    reader.require_finished();
    validate_result_for_decode(result);
    return result;
}

std::vector<std::byte> encode_network_list_result(const NetworkListResult& result) {
    if (result.networks.size() > max_network_list_entries) {
        throw std::length_error("network list contains too many entries");
    }

    std::vector<std::byte> output;
    output.reserve(2 + result.networks.size() * (63 + max_network_name_size));
    append_integer(output, static_cast<std::uint16_t>(result.networks.size()));
    std::vector<NetworkSummary> validated;
    validated.reserve(result.networks.size());

    for (const auto& summary : result.networks) {
        if (contains_network_id(validated, summary.network_id)) {
            throw std::invalid_argument("network list contains a duplicate id");
        }

        append_summary(output, summary);
        validated.push_back(summary);
    }

    return output;
}

NetworkListResult decode_network_list_result(const std::span<const std::byte> payload) {
    Reader reader(payload);
    const auto count = reader.read_integer<std::uint16_t>();

    if (count > max_network_list_entries) {
        throw std::runtime_error("network list contains too many entries");
    }

    NetworkListResult result;
    result.networks.reserve(count);

    for (std::uint16_t index = 0; index < count; ++index) {
        auto summary = read_summary(reader);

        if (contains_network_id(result.networks, summary.network_id)) {
            throw std::runtime_error("network list contains a duplicate id");
        }

        result.networks.push_back(std::move(summary));
    }

    reader.require_finished();
    return result;
}

std::vector<std::byte> encode_network_event(const NetworkEvent& event) {
    if (!is_known_network_event_kind(event.kind)) {
        throw std::invalid_argument("unknown network event kind");
    }

    require_network_id_for_encode(event.network_id);
    require_device_id_for_encode(event.device_id);
    std::vector<std::byte> output;
    output.reserve(network_event_size);
    append_integer(output, static_cast<std::uint8_t>(event.kind));
    append_array(output, event.network_id);
    append_array(output, event.device_id);
    return output;
}

std::vector<std::byte> encode_network_peer_state(const NetworkPeerState& state) {
    if (!valid_peer_state(state)) {
        throw std::invalid_argument("network peer state contains invalid addresses or peers");
    }

    std::vector<std::byte> output;
    output.reserve(35 + state.peers.size() *
                            (network_device_id_size + 5 + auth::public_key_size +
                             auth::encryption_public_key_size + 2 * auth::nonce_size +
                             auth::signature_size));
    append_array(output, state.network_id);
    append_integer(output, state.revision);
    append_integer(output, state.subnet_address);
    append_integer(output, state.prefix_length);
    append_integer(output, state.own_address);
    append_integer(output, static_cast<std::uint16_t>(state.peers.size()));
    for (const auto& peer : state.peers) {
        append_array(output, peer.device_id);
        append_integer(output, peer.ipv4_address);
        append_integer(output, static_cast<std::uint8_t>(peer.signed_key ? 1 : 0));
        if (peer.signed_key) {
            append_array(output, peer.signed_key->identity_public_key);
            append_array(output, peer.signed_key->encryption_public_key);
            append_array(output, peer.signed_key->client_nonce);
            append_array(output, peer.signed_key->server_nonce);
            append_array(output, peer.signed_key->signature);
        }
    }
    return output;
}

NetworkPeerState decode_network_peer_state(const std::span<const std::byte> payload) {
    Reader reader(payload);
    NetworkPeerState state;
    state.network_id = reader.read_array<network_id_size>();
    state.revision = reader.read_integer<std::uint64_t>();
    state.subnet_address = reader.read_integer<std::uint32_t>();
    state.prefix_length = reader.read_integer<std::uint8_t>();
    state.own_address = reader.read_integer<std::uint32_t>();
    const auto count = reader.read_integer<std::uint16_t>();
    if (count > max_network_peer_entries) {
        throw std::runtime_error("network peer state contains too many peers");
    }
    state.peers.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        NetworkPeer peer;
        peer.device_id = reader.read_array<network_device_id_size>();
        peer.ipv4_address = reader.read_integer<std::uint32_t>();
        const auto has_key = reader.read_integer<std::uint8_t>();
        if (has_key > 1) {
            throw std::runtime_error("network peer key flag is invalid");
        }
        if (has_key == 1) {
            auth::SignedDeviceKey key;
            key.identity_public_key = reader.read_array<auth::public_key_size>();
            key.encryption_public_key =
                reader.read_array<auth::encryption_public_key_size>();
            key.client_nonce = reader.read_array<auth::nonce_size>();
            key.server_nonce = reader.read_array<auth::nonce_size>();
            key.signature = reader.read_array<auth::signature_size>();
            peer.signed_key = key;
        }
        state.peers.push_back(std::move(peer));
    }
    reader.require_finished();
    if (!valid_peer_state(state)) {
        throw std::runtime_error("network peer state contains invalid addresses or peers");
    }
    return state;
}

std::vector<std::byte> encode_network_peer_revocation(
    const NetworkPeerRevocation& revocation) {
    require_network_id_for_encode(revocation.network_id);
    std::vector<std::byte> output;
    output.reserve(network_id_size + 8);
    append_array(output, revocation.network_id);
    append_integer(output, revocation.revision);
    return output;
}

NetworkPeerRevocation decode_network_peer_revocation(
    const std::span<const std::byte> payload) {
    Reader reader(payload);
    NetworkPeerRevocation revocation;
    revocation.network_id = reader.read_array<network_id_size>();
    revocation.revision = reader.read_integer<std::uint64_t>();
    reader.require_finished();
    require_network_id_for_decode(revocation.network_id);
    return revocation;
}

NetworkEvent decode_network_event(const std::span<const std::byte> payload) {
    if (payload.size() != network_event_size) {
        throw std::runtime_error("network event has an invalid size");
    }

    Reader reader(payload);
    NetworkEvent event;
    event.kind = static_cast<NetworkEventKind>(reader.read_integer<std::uint8_t>());
    event.network_id = reader.read_array<network_id_size>();
    event.device_id = reader.read_array<network_device_id_size>();
    reader.require_finished();

    if (!is_known_network_event_kind(event.kind)) {
        throw std::runtime_error("network event contains an unknown kind");
    }

    require_network_id_for_decode(event.network_id);
    require_device_id_for_decode(event.device_id);
    return event;
}

bool is_known_network_operation(const NetworkOperation operation) noexcept {
    switch (operation) {
        case NetworkOperation::create:
        case NetworkOperation::list:
        case NetworkOperation::join:
        case NetworkOperation::leave:
        case NetworkOperation::invite:
        case NetworkOperation::approve:
        case NetworkOperation::kick:
        case NetworkOperation::peer_state:
            return true;
    }

    return false;
}

bool is_known_network_result_code(const NetworkResultCode code) noexcept {
    switch (code) {
        case NetworkResultCode::success:
        case NetworkResultCode::pending_approval:
        case NetworkResultCode::invalid_request:
        case NetworkResultCode::not_found:
        case NetworkResultCode::already_exists:
        case NetworkResultCode::not_member:
        case NetworkResultCode::permission_denied:
        case NetworkResultCode::invite_required:
        case NetworkResultCode::target_not_found:
        case NetworkResultCode::limit_reached:
        case NetworkResultCode::conflict:
        case NetworkResultCode::internal_error:
            return true;
    }

    return false;
}

bool is_known_network_event_kind(const NetworkEventKind kind) noexcept {
    switch (kind) {
        case NetworkEventKind::invited:
        case NetworkEventKind::join_requested:
        case NetworkEventKind::member_joined:
        case NetworkEventKind::member_left:
        case NetworkEventKind::member_kicked:
            return true;
    }

    return false;
}

std::string_view network_operation_name(const NetworkOperation operation) noexcept {
    switch (operation) {
        case NetworkOperation::create:
            return "create";
        case NetworkOperation::list:
            return "list";
        case NetworkOperation::join:
            return "join";
        case NetworkOperation::leave:
            return "leave";
        case NetworkOperation::invite:
            return "invite";
        case NetworkOperation::approve:
            return "approve";
        case NetworkOperation::kick:
            return "kick";
        case NetworkOperation::peer_state:
            return "peer_state";
    }

    return "unknown";
}

std::string_view network_result_code_name(const NetworkResultCode code) noexcept {
    switch (code) {
        case NetworkResultCode::success:
            return "success";
        case NetworkResultCode::pending_approval:
            return "pending_approval";
        case NetworkResultCode::invalid_request:
            return "invalid_request";
        case NetworkResultCode::not_found:
            return "not_found";
        case NetworkResultCode::already_exists:
            return "already_exists";
        case NetworkResultCode::not_member:
            return "not_member";
        case NetworkResultCode::permission_denied:
            return "permission_denied";
        case NetworkResultCode::invite_required:
            return "invite_required";
        case NetworkResultCode::target_not_found:
            return "target_not_found";
        case NetworkResultCode::limit_reached:
            return "limit_reached";
        case NetworkResultCode::conflict:
            return "conflict";
        case NetworkResultCode::internal_error:
            return "internal_error";
    }

    return "unknown";
}

std::string_view network_event_kind_name(const NetworkEventKind kind) noexcept {
    switch (kind) {
        case NetworkEventKind::invited:
            return "invited";
        case NetworkEventKind::join_requested:
            return "join_requested";
        case NetworkEventKind::member_joined:
            return "member_joined";
        case NetworkEventKind::member_left:
            return "member_left";
        case NetworkEventKind::member_kicked:
            return "member_kicked";
    }

    return "unknown";
}

}
