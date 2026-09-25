#include "lanlink/protocol/network_messages.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
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

std::vector<std::byte> make_bytes(const std::initializer_list<unsigned int> values) {
    std::vector<std::byte> result;
    result.reserve(values.size());

    for (const auto value : values) {
        result.push_back(std::byte{static_cast<unsigned char>(value)});
    }

    return result;
}

lanlink::protocol::NetworkId network_id(const std::uint32_t seed) {
    lanlink::protocol::NetworkId result{};
    result[0] = std::byte{static_cast<unsigned char>(seed >> 24U)};
    result[1] = std::byte{static_cast<unsigned char>(seed >> 16U)};
    result[2] = std::byte{static_cast<unsigned char>(seed >> 8U)};
    result[3] = std::byte{static_cast<unsigned char>(seed)};
    result.back() = std::byte{0xa5};
    return result;
}

lanlink::protocol::NetworkDeviceId device_id(const std::uint32_t seed) {
    lanlink::protocol::NetworkDeviceId result{};

    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] =
            std::byte{static_cast<unsigned char>((seed + index * 31U) & 0xffU)};
    }

    result.back() = std::byte{0x5a};
    return result;
}

lanlink::protocol::NetworkSummary summary(const std::uint32_t seed) {
    lanlink::protocol::NetworkSummary result;
    result.network_id = network_id(seed);
    result.owner_device_id = device_id(seed + 1000U);
    result.role = seed % 2U == 0U ? lanlink::protocol::NetworkRole::owner
                                  : lanlink::protocol::NetworkRole::member;
    result.created_at_ms = 10'000 + seed;
    result.member_count = seed + 1U;
    result.name = "LAN " + std::to_string(seed);
    return result;
}

void test_constants_and_names() {
    using namespace lanlink::protocol;

    expect(network_id_size == 16, "network id size");
    expect(network_device_id_size == 32, "device id size");
    expect(max_network_name_size == 128, "network name limit");
    expect(max_network_list_entries == 1024, "network list limit");
    expect(max_network_peer_entries == 253, "network peer limit");

    constexpr std::array operations{
        NetworkOperation::create,
        NetworkOperation::list,
        NetworkOperation::join,
        NetworkOperation::leave,
        NetworkOperation::invite,
        NetworkOperation::approve,
        NetworkOperation::kick,
        NetworkOperation::peer_state,
    };

    for (const auto operation : operations) {
        expect(is_known_network_operation(operation), "known network operation");
        expect(network_operation_name(operation) != "unknown", "network operation name");
    }

    constexpr std::array result_codes{
        NetworkResultCode::success,
        NetworkResultCode::pending_approval,
        NetworkResultCode::invalid_request,
        NetworkResultCode::not_found,
        NetworkResultCode::already_exists,
        NetworkResultCode::not_member,
        NetworkResultCode::permission_denied,
        NetworkResultCode::invite_required,
        NetworkResultCode::target_not_found,
        NetworkResultCode::limit_reached,
        NetworkResultCode::conflict,
        NetworkResultCode::internal_error,
    };

    for (const auto code : result_codes) {
        expect(is_known_network_result_code(code), "known network result code");
        expect(network_result_code_name(code) != "unknown", "network result code name");
    }

    constexpr std::array event_kinds{
        NetworkEventKind::invited,
        NetworkEventKind::join_requested,
        NetworkEventKind::member_joined,
        NetworkEventKind::member_left,
        NetworkEventKind::member_kicked,
    };

    for (const auto kind : event_kinds) {
        expect(is_known_network_event_kind(kind), "known network event kind");
        expect(network_event_kind_name(kind) != "unknown", "network event kind name");
    }

    expect(!is_known_network_operation(static_cast<NetworkOperation>(0xff)),
           "unknown network operation");
    expect(!is_known_network_result_code(static_cast<NetworkResultCode>(0xffff)),
           "unknown network result code");
    expect(!is_known_network_event_kind(static_cast<NetworkEventKind>(0xff)),
           "unknown network event kind");
    expect(network_operation_name(static_cast<NetworkOperation>(0xff)) == "unknown",
           "unknown network operation name");
    expect(network_result_code_name(static_cast<NetworkResultCode>(0xffff)) == "unknown",
           "unknown network result code name");
    expect(network_event_kind_name(static_cast<NetworkEventKind>(0xff)) == "unknown",
           "unknown network event kind name");
}

void test_golden_payloads() {
    using namespace lanlink::protocol;

    const NetworkCreateRequest create{"LAN"};
    expect(encode_network_create_request(create) ==
               make_bytes({0x00, 0x03, 0x4c, 0x41, 0x4e}),
           "create request wire bytes");

    const NetworkSelectionRequest selection{network_id(0x01020304U)};
    const auto selection_bytes = encode_network_selection_request(selection);
    expect(selection_bytes.size() == network_id_size, "selection wire size");
    expect(std::equal(selection.network_id.begin(),
                      selection.network_id.end(),
                      selection_bytes.begin()),
           "selection wire bytes");

    const NetworkMemberRequest member{selection.network_id, device_id(7)};
    const auto member_bytes = encode_network_member_request(member);
    expect(member_bytes.size() == network_id_size + network_device_id_size,
           "member request wire size");
    expect(std::equal(member.device_id.begin(),
                      member.device_id.end(),
                      member_bytes.begin() + static_cast<std::ptrdiff_t>(network_id_size)),
           "member request device bytes");

    const NetworkOperationResult operation{
        NetworkOperation::join,
        NetworkResultCode::success,
        selection.network_id,
    };
    const auto operation_bytes = encode_network_operation_result(operation);
    expect(operation_bytes.size() == 19, "operation result wire size");
    expect(operation_bytes[0] == std::byte{0x03} &&
               operation_bytes[1] == std::byte{0x00} &&
               operation_bytes[2] == std::byte{0x00},
           "operation result wire prefix");

    const NetworkEvent event{
        NetworkEventKind::invited,
        selection.network_id,
        member.device_id,
    };
    const auto event_bytes = encode_network_event(event);
    expect(event_bytes.size() == 49, "event wire size");
    expect(event_bytes.front() == std::byte{0x01}, "event wire kind");

    expect(encode_network_list_request().empty(), "list request wire bytes");
    expect(encode_network_list_result({}).size() == 2, "empty list wire size");

    auto fixed = summary(9);
    fixed.role = NetworkRole::owner;
    fixed.created_at_ms = 0x0102030405060708LL;
    fixed.member_count = 0x01020304U;
    fixed.name = "LAN";
    const auto list_bytes = encode_network_list_result({{fixed}});
    expect(list_bytes.size() == 68, "list result wire size");
    expect(list_bytes[0] == std::byte{0x00} && list_bytes[1] == std::byte{0x01},
           "list result count");
    expect(list_bytes[50] == std::byte{0x01}, "list result role");
    expect(list_bytes[51] == std::byte{0x01} && list_bytes[58] == std::byte{0x08},
           "list result timestamp");
    expect(list_bytes[59] == std::byte{0x01} && list_bytes[62] == std::byte{0x04},
           "list result member count");
    expect(list_bytes[63] == std::byte{0x00} && list_bytes[64] == std::byte{0x03},
           "list result name length");
}

void test_round_trips() {
    using namespace lanlink::protocol;

    const NetworkCreateRequest create{std::string{"Panda \xec\x88\xb2"}};
    expect(decode_network_create_request(encode_network_create_request(create)) == create,
           "create request round trip");
    const NetworkCreateRequest maximum_name{std::string(max_network_name_size, 'x')};
    expect(decode_network_create_request(encode_network_create_request(maximum_name)) ==
               maximum_name,
           "maximum network name round trip");

    const NetworkListRequest list_request;
    expect(decode_network_list_request(encode_network_list_request(list_request)) == list_request,
           "list request round trip");

    const NetworkSelectionRequest selection{network_id(12)};
    expect(decode_network_selection_request(encode_network_selection_request(selection)) ==
               selection,
           "selection request round trip");

    const NetworkMemberRequest member{selection.network_id, device_id(12)};
    expect(decode_network_member_request(encode_network_member_request(member)) == member,
           "member request round trip");

    const std::array results{
        NetworkOperationResult{
            NetworkOperation::create, NetworkResultCode::success, network_id(1)},
        NetworkOperationResult{
            NetworkOperation::create, NetworkResultCode::conflict, {}},
        NetworkOperationResult{
            NetworkOperation::list, NetworkResultCode::internal_error, {}},
        NetworkOperationResult{
            NetworkOperation::join, NetworkResultCode::pending_approval, network_id(2)},
        NetworkOperationResult{
            NetworkOperation::leave, NetworkResultCode::success, network_id(3)},
        NetworkOperationResult{
            NetworkOperation::invite, NetworkResultCode::target_not_found, network_id(4)},
        NetworkOperationResult{
            NetworkOperation::approve, NetworkResultCode::permission_denied, network_id(5)},
        NetworkOperationResult{
            NetworkOperation::kick, NetworkResultCode::not_member, network_id(6)},
        NetworkOperationResult{
            NetworkOperation::peer_state, NetworkResultCode::not_member, network_id(7)},
    };

    for (const auto& result : results) {
        expect(decode_network_operation_result(encode_network_operation_result(result)) == result,
               "operation result round trip");
    }

    NetworkListResult list;
    list.networks = {summary(1), summary(2), summary(3)};
    expect(decode_network_list_result(encode_network_list_result(list)) == list,
           "list result round trip");
    expect(decode_network_list_result(encode_network_list_result({})).networks.empty(),
           "empty list result round trip");

    constexpr std::array kinds{
        NetworkEventKind::invited,
        NetworkEventKind::join_requested,
        NetworkEventKind::member_joined,
        NetworkEventKind::member_left,
        NetworkEventKind::member_kicked,
    };

    for (const auto kind : kinds) {
        const NetworkEvent event{kind, network_id(8), device_id(8)};
        expect(decode_network_event(encode_network_event(event)) == event,
               "network event round trip");
    }

    const NetworkPeerState peers{network_id(9), 0x0102030405060708ULL,
                                 0x0a400000U, 24, 0x0a400001U,
                                 {{device_id(2), 0x0a400002U},
                                  {device_id(3), 0x0a400003U}}};
    const auto wire = encode_network_peer_state(peers);
    expect(decode_network_peer_state(wire) == peers, "peer snapshot round trip");
    expect(wire.size() == 35 + 2 * 37 && wire[16] == std::byte{1} &&
               wire[23] == std::byte{8} && wire[24] == std::byte{10} &&
               wire[26] == std::byte{0} && wire[28] == std::byte{24} &&
               wire[33] == std::byte{0} && wire[34] == std::byte{2},
           "peer snapshot network-order wire fields");
    const NetworkPeerRevocation revoked{network_id(9), 0x0102030405060708ULL};
    expect(decode_network_peer_revocation(encode_network_peer_revocation(revoked)) ==
               revoked,
           "peer revocation round trip");
}

void test_peer_state_validation() {
    using namespace lanlink::protocol;

    NetworkPeerState state{network_id(1), 1, 0x0a400000U, 24,
                           0x0a400001U, {{device_id(2), 0x0a400002U}}};
    auto invalid = state;
    invalid.prefix_length = 16;
    expect_error([&] { static_cast<void>(encode_network_peer_state(invalid)); },
                 "peer subnet prefix validated");
    invalid = state;
    invalid.subnet_address = 0x0a400001U;
    expect_error([&] { static_cast<void>(encode_network_peer_state(invalid)); },
                 "peer subnet alignment validated");
    invalid = state;
    invalid.own_address = 0x0a4000ffU;
    expect_error([&] { static_cast<void>(encode_network_peer_state(invalid)); },
                 "broadcast cannot be own address");
    invalid = state;
    invalid.peers.push_back({device_id(2), 0x0a400003U});
    expect_error([&] { static_cast<void>(encode_network_peer_state(invalid)); },
                 "duplicate peer identity rejected");
    invalid = state;
    invalid.peers.push_back({device_id(3), 0x0a400002U});
    expect_error([&] { static_cast<void>(encode_network_peer_state(invalid)); },
                 "duplicate peer address rejected");
    invalid = state;
    invalid.peers[0].ipv4_address = 0x0a400001U;
    expect_error([&] { static_cast<void>(encode_network_peer_state(invalid)); },
                 "own and peer address cannot overlap");
    invalid = state;
    invalid.peers[0].ipv4_address = 0x0a400102U;
    expect_error([&] { static_cast<void>(encode_network_peer_state(invalid)); },
                 "peer from another subnet rejected");

    const auto wire = encode_network_peer_state(state);
    auto truncated = wire;
    truncated.pop_back();
    expect_error([&] { static_cast<void>(decode_network_peer_state(truncated)); },
                 "truncated peer state rejected");
    auto trailing = wire;
    trailing.push_back(std::byte{0});
    expect_error([&] { static_cast<void>(decode_network_peer_state(trailing)); },
                 "trailing peer state bytes rejected");
    auto bad_key_flag = wire;
    bad_key_flag[71] = std::byte{2};
    expect_error([&] { static_cast<void>(decode_network_peer_state(bad_key_flag)); },
                 "invalid signed key presence flag rejected");
    auto signed_state = state;
    lanlink::auth::SignedDeviceKey signed_key;
    signed_key.identity_public_key.fill(std::byte{0x51});
    signed_key.encryption_public_key.fill(std::byte{0x52});
    signed_key.client_nonce.fill(std::byte{0x53});
    signed_key.server_nonce.fill(std::byte{0x54});
    signed_key.signature.fill(std::byte{0x55});
    signed_state.peers.front().signed_key = signed_key;
    signed_state.peers.resize(1);
    const auto signed_wire = encode_network_peer_state(signed_state);
    expect(decode_network_peer_state(signed_wire) == signed_state,
           "signed peer key survives network wire round trip");
    auto short_signed = signed_wire;
    short_signed.pop_back();
    expect_error([&] { static_cast<void>(decode_network_peer_state(short_signed)); },
                 "truncated signed key rejected");
    auto many = wire;
    many[33] = std::byte{0};
    many[34] = std::byte{254};
    expect_error([&] { static_cast<void>(decode_network_peer_state(many)); },
                 "oversized peer count rejected before allocation");
    auto zero_id = wire;
    std::fill_n(zero_id.begin(), network_id_size, std::byte{0});
    expect_error([&] { static_cast<void>(decode_network_peer_state(zero_id)); },
                 "zero peer state network rejected");
    auto bad_ip = wire;
    bad_ip.back() = std::byte{255};
    expect_error([&] { static_cast<void>(decode_network_peer_state(bad_ip)); },
                 "broadcast peer address rejected on decode");

    expect_error([] { static_cast<void>(encode_network_peer_revocation({})); },
                 "zero revocation network rejected");
    auto revoked = encode_network_peer_revocation({network_id(1), 2});
    revoked.push_back(std::byte{0});
    expect_error([&] { static_cast<void>(decode_network_peer_revocation(revoked)); },
                 "trailing revocation bytes rejected");
}

void test_request_validation() {
    using namespace lanlink::protocol;

    expect_error([] {
        static_cast<void>(encode_network_create_request({""}));
    }, "empty network name rejected");
    expect_error([] {
        static_cast<void>(encode_network_create_request({std::string(129, 'x')}));
    }, "long network name rejected");
    expect_error([] {
        static_cast<void>(encode_network_create_request({"bad\nname"}));
    }, "network name control rejected");
    expect_error([] {
        static_cast<void>(encode_network_create_request({std::string{"\xc0\xaf"}}));
    }, "overlong UTF-8 rejected");
    expect_error([] {
        static_cast<void>(encode_network_create_request({std::string{"\xe2\x82"}}));
    }, "truncated UTF-8 rejected");

    expect_error([] {
        static_cast<void>(decode_network_create_request(make_bytes({0x00, 0x03, 0x41})));
    }, "truncated create request rejected");
    expect_error([] {
        static_cast<void>(
            decode_network_create_request(make_bytes({0x00, 0x01, 0x41, 0x42})));
    }, "trailing create request bytes rejected");
    expect_error([] {
        static_cast<void>(
            decode_network_create_request(make_bytes({0x00, 0x02, 0xc0, 0xaf})));
    }, "invalid create request UTF-8 rejected");
    expect_error([] {
        static_cast<void>(decode_network_list_request(make_bytes({0x00})));
    }, "non-empty list request rejected");

    expect_error([] {
        static_cast<void>(encode_network_selection_request({}));
    }, "zero selection id rejected");
    expect_error([] {
        static_cast<void>(decode_network_selection_request(std::vector<std::byte>(15)));
    }, "short selection request rejected");
    expect_error([] {
        static_cast<void>(decode_network_selection_request(std::vector<std::byte>(17)));
    }, "long selection request rejected");
    expect_error([] {
        static_cast<void>(decode_network_selection_request(std::vector<std::byte>(16)));
    }, "zero selection wire id rejected");

    NetworkMemberRequest request{network_id(1), device_id(1)};
    auto zero_network = request;
    zero_network.network_id.fill(std::byte{0});
    expect_error([&zero_network] {
        static_cast<void>(encode_network_member_request(zero_network));
    }, "zero member network id rejected");

    auto zero_device = request;
    zero_device.device_id.fill(std::byte{0});
    expect_error([&zero_device] {
        static_cast<void>(encode_network_member_request(zero_device));
    }, "zero member device id rejected");
    expect_error([] {
        static_cast<void>(decode_network_member_request(std::vector<std::byte>(47)));
    }, "short member request rejected");

    auto member_wire = encode_network_member_request(request);
    std::fill_n(member_wire.begin(), network_id_size, std::byte{0});
    expect_error([&member_wire] {
        static_cast<void>(decode_network_member_request(member_wire));
    }, "zero member wire network id rejected");

    member_wire = encode_network_member_request(request);
    std::fill(member_wire.begin() + static_cast<std::ptrdiff_t>(network_id_size),
              member_wire.end(),
              std::byte{0});
    expect_error([&member_wire] {
        static_cast<void>(decode_network_member_request(member_wire));
    }, "zero member wire device id rejected");
}

void test_operation_result_validation() {
    using namespace lanlink::protocol;

    expect_error([] {
        static_cast<void>(encode_network_operation_result(
            {static_cast<NetworkOperation>(0xff), NetworkResultCode::success, network_id(1)}));
    }, "unknown operation encode rejected");
    expect_error([] {
        static_cast<void>(encode_network_operation_result(
            {NetworkOperation::join,
             static_cast<NetworkResultCode>(0xffff),
             network_id(1)}));
    }, "unknown result code encode rejected");
    expect_error([] {
        static_cast<void>(encode_network_operation_result(
            {NetworkOperation::leave, NetworkResultCode::pending_approval, network_id(1)}));
    }, "invalid pending operation rejected");
    expect_error([] {
        static_cast<void>(encode_network_operation_result(
            {NetworkOperation::create, NetworkResultCode::success, {}}));
    }, "successful create requires id");
    expect_error([] {
        static_cast<void>(encode_network_operation_result(
            {NetworkOperation::create, NetworkResultCode::conflict, network_id(1)}));
    }, "failed create forbids id");
    expect_error([] {
        static_cast<void>(encode_network_operation_result(
            {NetworkOperation::list, NetworkResultCode::success, network_id(1)}));
    }, "list operation forbids id");

    auto encoded = encode_network_operation_result(
        {NetworkOperation::join, NetworkResultCode::success, network_id(1)});
    auto unknown_operation = encoded;
    unknown_operation[0] = std::byte{0xff};
    expect_error([&unknown_operation] {
        static_cast<void>(decode_network_operation_result(unknown_operation));
    }, "unknown operation decode rejected");

    auto unknown_code = encoded;
    unknown_code[1] = std::byte{0xff};
    unknown_code[2] = std::byte{0xff};
    expect_error([&unknown_code] {
        static_cast<void>(decode_network_operation_result(unknown_code));
    }, "unknown result code decode rejected");

    auto invalid_pending = encode_network_operation_result(
        {NetworkOperation::join, NetworkResultCode::pending_approval, network_id(1)});
    invalid_pending[0] = std::byte{static_cast<std::uint8_t>(NetworkOperation::leave)};
    expect_error([&invalid_pending] {
        static_cast<void>(decode_network_operation_result(invalid_pending));
    }, "invalid pending operation decode rejected");

    std::fill(encoded.begin() + 3, encoded.end(), std::byte{0});
    expect_error([&encoded] {
        static_cast<void>(decode_network_operation_result(encoded));
    }, "zero operation network id rejected");
    expect_error([] {
        static_cast<void>(decode_network_operation_result(std::vector<std::byte>(18)));
    }, "short operation result rejected");
}

void test_list_validation() {
    using namespace lanlink::protocol;

    auto invalid = summary(1);
    invalid.network_id.fill(std::byte{0});
    expect_error([&invalid] {
        static_cast<void>(encode_network_list_result({{invalid}}));
    }, "zero summary network id rejected");

    invalid = summary(1);
    invalid.owner_device_id.fill(std::byte{0});
    expect_error([&invalid] {
        static_cast<void>(encode_network_list_result({{invalid}}));
    }, "zero summary owner id rejected");

    invalid = summary(1);
    invalid.role = static_cast<NetworkRole>(0xff);
    expect_error([&invalid] {
        static_cast<void>(encode_network_list_result({{invalid}}));
    }, "unknown summary role rejected");

    invalid = summary(1);
    invalid.created_at_ms = -1;
    expect_error([&invalid] {
        static_cast<void>(encode_network_list_result({{invalid}}));
    }, "negative summary timestamp rejected");

    invalid = summary(1);
    invalid.member_count = 0;
    expect_error([&invalid] {
        static_cast<void>(encode_network_list_result({{invalid}}));
    }, "zero summary member count rejected");

    const auto duplicate = summary(3);
    expect_error([&duplicate] {
        static_cast<void>(encode_network_list_result({{duplicate, duplicate}}));
    }, "duplicate summary id encode rejected");

    NetworkListResult maximum;
    maximum.networks.reserve(max_network_list_entries);

    for (std::uint32_t index = 0; index < max_network_list_entries; ++index) {
        maximum.networks.push_back(summary(index + 1U));
    }

    const auto maximum_bytes = encode_network_list_result(maximum);
    expect(decode_network_list_result(maximum_bytes) == maximum, "maximum list round trip");

    auto too_many = maximum;
    too_many.networks.push_back(summary(max_network_list_entries + 1U));
    expect_error([&too_many] {
        static_cast<void>(encode_network_list_result(too_many));
    }, "oversized list encode rejected");
    expect_error([] {
        static_cast<void>(decode_network_list_result(make_bytes({0x04, 0x01})));
    }, "oversized list decode rejected");

    const NetworkListResult pair{{summary(11), summary(12)}};
    const auto valid = encode_network_list_result(pair);
    auto truncated = valid;
    truncated.pop_back();
    expect_error([&truncated] {
        static_cast<void>(decode_network_list_result(truncated));
    }, "truncated list rejected");

    auto trailing = valid;
    trailing.push_back(std::byte{0});
    expect_error([&trailing] {
        static_cast<void>(decode_network_list_result(trailing));
    }, "trailing list bytes rejected");

    auto duplicate_wire = valid;
    const auto second_id_offset = 2 + 63 + pair.networks.front().name.size();
    std::copy_n(duplicate_wire.begin() + 2,
                network_id_size,
                duplicate_wire.begin() + static_cast<std::ptrdiff_t>(second_id_offset));
    expect_error([&duplicate_wire] {
        static_cast<void>(decode_network_list_result(duplicate_wire));
    }, "duplicate summary id decode rejected");

    auto unknown_role = encode_network_list_result({{summary(1)}});
    unknown_role[50] = std::byte{0xff};
    expect_error([&unknown_role] {
        static_cast<void>(decode_network_list_result(unknown_role));
    }, "unknown summary role decode rejected");

    auto timestamp_overflow = encode_network_list_result({{summary(1)}});
    timestamp_overflow[51] = std::byte{0x80};
    expect_error([&timestamp_overflow] {
        static_cast<void>(decode_network_list_result(timestamp_overflow));
    }, "overflow summary timestamp rejected");

    auto zero_members = encode_network_list_result({{summary(1)}});
    std::fill(zero_members.begin() + 59, zero_members.begin() + 63, std::byte{0});
    expect_error([&zero_members] {
        static_cast<void>(decode_network_list_result(zero_members));
    }, "zero summary member count decode rejected");

    auto empty_name = encode_network_list_result({{summary(1)}});
    empty_name[63] = std::byte{0};
    empty_name[64] = std::byte{0};
    empty_name.resize(65);
    expect_error([&empty_name] {
        static_cast<void>(decode_network_list_result(empty_name));
    }, "empty summary name decode rejected");

    auto invalid_utf8 = summary(1);
    invalid_utf8.name = std::string{"\xc0\xaf"};
    expect_error([&invalid_utf8] {
        static_cast<void>(encode_network_list_result({{invalid_utf8}}));
    }, "invalid summary UTF-8 rejected");
}

void test_event_validation() {
    using namespace lanlink::protocol;

    expect_error([] {
        static_cast<void>(encode_network_event(
            {static_cast<NetworkEventKind>(0xff), network_id(1), device_id(1)}));
    }, "unknown event kind encode rejected");
    expect_error([] {
        static_cast<void>(encode_network_event({NetworkEventKind::invited, {}, device_id(1)}));
    }, "zero event network id rejected");
    expect_error([] {
        static_cast<void>(encode_network_event({NetworkEventKind::invited, network_id(1), {}}));
    }, "zero event device id rejected");

    auto encoded = encode_network_event(
        {NetworkEventKind::member_joined, network_id(1), device_id(1)});
    encoded.front() = std::byte{0xff};
    expect_error([&encoded] {
        static_cast<void>(decode_network_event(encoded));
    }, "unknown event kind decode rejected");
    expect_error([] {
        static_cast<void>(decode_network_event(std::vector<std::byte>(48)));
    }, "short event rejected");

    encoded = encode_network_event(
        {NetworkEventKind::member_joined, network_id(1), device_id(1)});
    std::fill(encoded.begin() + 1,
              encoded.begin() + 1 + static_cast<std::ptrdiff_t>(network_id_size),
              std::byte{0});
    expect_error([&encoded] {
        static_cast<void>(decode_network_event(encoded));
    }, "zero event wire network id rejected");
}

}

int main() {
    test_constants_and_names();
    test_golden_payloads();
    test_round_trips();
    test_peer_state_validation();
    test_request_validation();
    test_operation_result_validation();
    test_list_validation();
    test_event_validation();

    if (failures != 0) {
        std::cerr << failures << " test failure(s)\n";
        return 1;
    }

    std::cout << "all network protocol tests passed\n";
    return 0;
}
