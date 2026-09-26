#include "lanlink/auth/handshake.hpp"
#include "lanlink/auth/network_keys.hpp"
#include "lanlink/protocol/codec.hpp"
#include "lanlink/protocol/stream_decoder.hpp"
#include "lanlink/relay/network_control_channel.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
                      ("lanlink-network-control-tests-" + std::to_string(suffix));
    std::filesystem::create_directories(path);
    return path;
}

lanlink::auth::DeviceId device_id(const unsigned int seed) {
    lanlink::auth::DeviceId result{};
    result[0] = static_cast<std::byte>(seed);
    result.back() = std::byte{0xaa};
    return result;
}

lanlink::protocol::NetworkId network_id(const unsigned int seed) {
    lanlink::protocol::NetworkId result{};
    result[0] = static_cast<std::byte>(seed);
    result.back() = std::byte{0x99};
    return result;
}

lanlink::protocol::Frame request(const lanlink::protocol::MessageType type,
                                 const std::uint32_t request_id,
                                 std::vector<std::byte> payload) {
    return {type, request_id, std::move(payload)};
}

void expect_operation(const lanlink::relay::ControlDispatch& dispatch,
                      const std::uint32_t request_id,
                      const lanlink::protocol::NetworkOperation operation,
                      const lanlink::protocol::NetworkResultCode code,
                      const lanlink::protocol::NetworkId& id,
                      const std::string_view name) {
    using namespace lanlink::protocol;

    expect(dispatch.response.type == MessageType::network_operation_result &&
               dispatch.response.request_id == request_id,
           std::string(name) + " response frame");

    if (dispatch.response.type == MessageType::network_operation_result) {
        expect(decode_network_operation_result(dispatch.response.payload) ==
                   NetworkOperationResult{operation, code, id},
               name);
    }

    const auto wire = encode_frame(dispatch.response);
    const auto parsed = decode_frame(wire);
    expect(parsed.complete() && parsed.frame == dispatch.response,
           std::string(name) + " response wire round trip");
}

void expect_event(const lanlink::relay::RoutedControlFrame& routed,
                  const lanlink::auth::DeviceId& recipient,
                  const lanlink::protocol::NetworkEventKind kind,
                  const lanlink::protocol::NetworkId& id,
                  const lanlink::auth::DeviceId& subject,
                  const std::string_view name) {
    using namespace lanlink::protocol;

    expect(routed.recipient_device_id == recipient &&
               routed.frame.type == MessageType::network_event &&
               routed.frame.request_id == 0 &&
               decode_network_event(routed.frame.payload) ==
                   NetworkEvent{kind, id, subject},
           name);

    const auto wire = encode_frame(routed.frame);
    const auto parsed = decode_frame(wire);
    expect(parsed.complete() && parsed.frame == routed.frame,
           std::string(name) + " event wire round trip");
}

void expect_peer_state(const lanlink::relay::RoutedControlFrame& routed,
                       const lanlink::auth::DeviceId& recipient,
                       const lanlink::protocol::NetworkId& id,
                       const std::uint32_t own_address,
                       const std::size_t peer_count,
                       const std::uint64_t revision,
                       const std::string_view name) {
    using namespace lanlink::protocol;
    expect(routed.recipient_device_id == recipient &&
               routed.frame.type == MessageType::network_peer_state_update &&
               routed.frame.request_id == 0,
           std::string(name) + " routed update");
    if (routed.frame.type == MessageType::network_peer_state_update) {
        const auto state = decode_network_peer_state(routed.frame.payload);
        expect(state.network_id == id && state.revision == revision &&
                   state.subnet_address == 0x0a400000U &&
                   state.prefix_length == 24 &&
                   state.own_address == own_address &&
                   state.peers.size() == peer_count,
               name);
    }
}

void expect_revocation(const lanlink::relay::RoutedControlFrame& routed,
                       const lanlink::auth::DeviceId& recipient,
                       const lanlink::protocol::NetworkId& id,
                       const std::uint64_t revision,
                       const std::string_view name) {
    using namespace lanlink::protocol;
    expect(routed.recipient_device_id == recipient &&
               routed.frame.type == MessageType::network_peer_state_revoked &&
               routed.frame.request_id == 0 &&
               decode_network_peer_revocation(routed.frame.payload) ==
                   NetworkPeerRevocation{id, revision},
           name);
}

void test_management_flow(const std::filesystem::path& directory) {
    using namespace lanlink;

    storage::RelayStore store(directory / "management.db");
    const auto owner = device_id(1);
    const auto member = device_id(2);
    const auto guest = device_id(3);
    const auto id = network_id(9);
    relay::NetworkService service(store, [id] { return id; });
    relay::NetworkControlChannel channel(service, [] { return 1234; });

    const auto create = channel.handle_authenticated(
        owner,
        request(protocol::MessageType::network_create_request,
                100,
                protocol::encode_network_create_request({"Panda LAN"})));
    expect_operation(create,
                     100,
                     protocol::NetworkOperation::create,
                     protocol::NetworkResultCode::success,
                     id,
                     "owner create");
    expect(create.events.size() == 1, "create publishes owner peer state");
    if (create.events.size() == 1) {
        expect_peer_state(create.events.front(), owner, id, 0x0a400001U, 0, 1,
                          "owner sees assigned address");
    }

    const auto list = channel.handle_authenticated(
        owner, request(protocol::MessageType::network_list_request, 101, {}));
    expect(list.response.type == protocol::MessageType::network_list_result &&
               list.response.request_id == 101 && list.events.empty(),
           "list frame correlation");
    const auto listed = protocol::decode_network_list_result(list.response.payload);
    expect(listed.networks.size() == 1 && listed.networks.front().network_id == id &&
               listed.networks.front().role == protocol::NetworkRole::owner,
           "owner list includes network");

    channel.note_authenticated(member);
    channel.note_authenticated(guest);
    expect(store.find_device(member).has_value() && store.find_device(guest).has_value(),
           "authenticated targets registered before requests");

    const auto selection = protocol::encode_network_selection_request({id});
    const auto membership = protocol::encode_network_member_request({id, member});
    const auto guest_membership = protocol::encode_network_member_request({id, guest});

    const auto owner_peers = channel.handle_authenticated(
        owner, request(protocol::MessageType::network_peer_state_request, 103, selection));
    expect(owner_peers.response.type == protocol::MessageType::network_peer_state_result &&
               owner_peers.response.request_id == 103 &&
               protocol::decode_network_peer_state(owner_peers.response.payload).own_address ==
                   0x0a400001U,
           "owner can request current peer state");
    const auto outsider_peers = channel.handle_authenticated(
        guest, request(protocol::MessageType::network_peer_state_request, 103, selection));
    expect_operation(outsider_peers, 103, protocol::NetworkOperation::peer_state,
                     protocol::NetworkResultCode::not_member, id,
                     "nonmember cannot see peer addresses");

    const auto join = channel.handle_authenticated(
        member, request(protocol::MessageType::network_join_request, 104, selection));
    expect_operation(join,
                     104,
                     protocol::NetworkOperation::join,
                     protocol::NetworkResultCode::pending_approval,
                     id,
                     "member pending join");
    expect(join.events.size() == 1, "join routed to owner");
    if (join.events.size() == 1) {
        expect_event(join.events.front(),
                     owner,
                     protocol::NetworkEventKind::join_requested,
                     id,
                     member,
                     "owner sees join request");
    }

    const auto denied = channel.handle_authenticated(
        guest, request(protocol::MessageType::network_approve_request, 105, membership));
    expect_operation(denied,
                     105,
                     protocol::NetworkOperation::approve,
                     protocol::NetworkResultCode::permission_denied,
                     id,
                     "guest approval denied");
    expect(denied.events.empty(), "unauthorized approval has no events");

    const auto approved = channel.handle_authenticated(
        owner, request(protocol::MessageType::network_approve_request, 106, membership));
    expect_operation(approved,
                     106,
                     protocol::NetworkOperation::approve,
                     protocol::NetworkResultCode::success,
                     id,
                     "owner approves member");
    expect(approved.events.size() == 3, "approval notifies member and syncs both peers");
    if (approved.events.size() == 3) {
        expect_event(approved.events.front(),
                     member,
                     protocol::NetworkEventKind::member_joined,
                     id,
                     member,
                     "member sees approval");
        expect_peer_state(approved.events[1], owner, id, 0x0a400001U, 1, 2,
                          "owner sees approved member address");
        expect_peer_state(approved.events[2], member, id, 0x0a400002U, 1, 2,
                          "new member sees owner address");
    }

    const auto invited = channel.handle_authenticated(
        owner, request(protocol::MessageType::network_invite_request, 107, guest_membership));
    expect_operation(invited,
                     107,
                     protocol::NetworkOperation::invite,
                     protocol::NetworkResultCode::success,
                     id,
                     "owner invites guest");
    expect(invited.events.size() == 1, "invitation is routed");
    if (invited.events.size() == 1) {
        expect_event(invited.events.front(),
                     guest,
                     protocol::NetworkEventKind::invited,
                     id,
                     guest,
                     "guest receives invitation");
    }

    const auto joined = channel.handle_authenticated(
        guest, request(protocol::MessageType::network_join_request, 108, selection));
    expect_operation(joined,
                     108,
                     protocol::NetworkOperation::join,
                     protocol::NetworkResultCode::success,
                     id,
                     "invited guest joins");
    expect(joined.events.size() == 5, "join notifies and syncs all three members");
    if (joined.events.size() == 5) {
        expect_peer_state(joined.events[2], owner, id, 0x0a400001U, 2, 3,
                          "owner sees joined guest");
        expect_peer_state(joined.events[3], member, id, 0x0a400002U, 2, 3,
                          "member sees joined guest");
        expect_peer_state(joined.events[4], guest, id, 0x0a400003U, 2, 3,
                          "guest receives full peer snapshot");
    }

    const auto initial = channel.initial_peer_states(member);
    expect(initial.size() == 1, "reconnected member receives current snapshot");
    if (initial.size() == 1) {
        expect_peer_state(initial.front(), member, id, 0x0a400002U, 2, 3,
                          "reconnected member address restored");
    }

    const auto kicked = channel.handle_authenticated(
        owner, request(protocol::MessageType::network_kick_request, 109, membership));
    expect_operation(kicked,
                     109,
                     protocol::NetworkOperation::kick,
                     protocol::NetworkResultCode::success,
                     id,
                     "owner kicks member");
    expect(kicked.events.size() == 5, "kick notifies and revokes removed member");
    if (kicked.events.size() == 5) {
        expect_event(kicked.events.front(),
                     member,
                     protocol::NetworkEventKind::member_kicked,
                     id,
                     member,
                     "kicked member notified");
        expect_event(kicked.events[1],
                     guest,
                     protocol::NetworkEventKind::member_kicked,
                     id,
                     member,
                     "other member notified");
        expect_peer_state(kicked.events[2], owner, id, 0x0a400001U, 1, 4,
                          "owner drops kicked peer");
        expect_peer_state(kicked.events[3], guest, id, 0x0a400003U, 1, 4,
                          "guest drops kicked peer");
        expect_revocation(kicked.events[4], member, id, 4,
                          "kicked member address state revoked");
    }
    expect(channel.initial_peer_states(member).empty(),
           "kicked member receives no snapshot on reconnect");

    const auto left = channel.handle_authenticated(
        guest, request(protocol::MessageType::network_leave_request, 110, selection));
    expect_operation(left,
                     110,
                     protocol::NetworkOperation::leave,
                     protocol::NetworkResultCode::success,
                     id,
                     "guest leaves");
    expect(left.events.size() == 3, "leave notifies owner and revokes guest state");
    if (left.events.size() == 3) {
        expect_peer_state(left.events[1], owner, id, 0x0a400001U, 0, 5,
                          "owner drops departed guest");
        expect_revocation(left.events[2], guest, id, 5,
                          "departed guest address state revoked");
    }
    expect(channel.initial_peer_states(guest).empty(),
           "departed guest receives no snapshot on reconnect");

    const auto owner_leave = channel.handle_authenticated(
        owner, request(protocol::MessageType::network_leave_request, 111, selection));
    expect_operation(owner_leave,
                     111,
                     protocol::NetworkOperation::leave,
                     protocol::NetworkResultCode::permission_denied,
                     id,
                     "owner cannot leave");

    const auto stored_owner = store.find_device(owner);
    expect(stored_owner && stored_owner->last_seen_at_ms == 1234,
           "control operations record authenticated actor");
}

void test_invalid_frames(const std::filesystem::path& directory) {
    using namespace lanlink;

    storage::RelayStore store(directory / "invalid.db");
    relay::NetworkService service(store);
    relay::NetworkControlChannel channel(service, [] { return 1234; });
    const auto owner = device_id(4);
    const auth::DeviceId anonymous{};

    expect_error([&] {
        channel.note_authenticated(anonymous);
    }, "anonymous connection registration rejected");

    expect_error([&] {
        static_cast<void>(channel.handle_authenticated(
            anonymous, request(protocol::MessageType::network_list_request, 1, {})));
    }, "anonymous control request rejected");
    expect_error([&] {
        static_cast<void>(channel.handle_authenticated(
            owner, request(protocol::MessageType::network_list_request, 0, {})));
    }, "zero control request id rejected");
    expect_error([&] {
        static_cast<void>(channel.handle_authenticated(
            owner, request(protocol::MessageType::client_auth, 2, {})));
    }, "auth message after authentication rejected");
    expect_error([&] {
        static_cast<void>(channel.handle_authenticated(
            owner, request(protocol::MessageType::network_event, 3, {})));
    }, "server event as client request rejected");
    expect_error([&] {
        static_cast<void>(channel.handle_authenticated(
            owner, request(protocol::MessageType::network_peer_state_update, 4, {})));
    }, "server peer update as client request rejected");

    const auto malformed_list = channel.handle_authenticated(
        owner,
        request(protocol::MessageType::network_list_request, 21, {std::byte{0xff}}));
    expect(malformed_list.response ==
               protocol::Frame{protocol::MessageType::error, 21, {}} &&
               malformed_list.events.empty(),
           "malformed list returns correlated error");
    const auto malformed_selection = channel.handle_authenticated(
        owner,
        request(protocol::MessageType::network_join_request,
                22,
                std::vector<std::byte>(protocol::network_id_size)));
    expect(malformed_selection.response ==
               protocol::Frame{protocol::MessageType::error, 22, {}} &&
               malformed_selection.events.empty(),
           "zero network id returns correlated error");
    const auto malformed_member = channel.handle_authenticated(
        owner,
        request(protocol::MessageType::network_invite_request, 23, {std::byte{0}}));
    expect(malformed_member.response ==
               protocol::Frame{protocol::MessageType::error, 23, {}} &&
               malformed_member.events.empty(),
           "truncated member request returns correlated error");
    const auto malformed_peers = channel.handle_authenticated(
        owner,
        request(protocol::MessageType::network_peer_state_request, 25,
                std::vector<std::byte>(protocol::network_id_size)));
    expect(malformed_peers.response ==
               protocol::Frame{protocol::MessageType::error, 25, {}} &&
               malformed_peers.events.empty(),
           "zero peer state network returns correlated error");
    expect(!store.find_device(owner), "rejected malformed requests have no storage effects");

    relay::NetworkControlChannel bad_clock(service, [] { return -1; });
    expect_error([&] {
        bad_clock.note_authenticated(owner);
    }, "invalid clock prevents device registration");
    const auto clock_error = bad_clock.handle_authenticated(
        owner, request(protocol::MessageType::network_list_request, 24, {}));
    expect(clock_error.response ==
               protocol::Frame{protocol::MessageType::error, 24, {}},
           "invalid clock returns correlated error");
}

void test_authenticated_identity(const std::filesystem::path& directory) {
    using namespace lanlink;

    const auto owner_identity = auth::DeviceIdentity::load_or_create(directory / "owner.key");
    const auto guest_identity = auth::DeviceIdentity::load_or_create(directory / "guest.key");
    const auto token = auth::AuthToken::from_secret(
        "lanlink-network-control-channel-test-secret");
    auth::ClientHandshake owner_client(owner_identity, token);
    auth::ServerHandshake owner_server(
        token,
        42,
        auth::ServerHandshake::Clock::now() + std::chrono::minutes{1});

    const auto hello = owner_client.begin(9);
    const auto challenge = owner_server.handle_hello(hello);
    const auto proof = owner_client.handle_challenge(challenge);
    const auto accepted = owner_server.handle_proof(proof);
    expect(owner_client.handle_result(accepted) && owner_server.authenticated(),
           "handshake authenticates owner");
    expect(owner_server.bound_to(42,
                                 owner_identity.device_id(),
                                 owner_server.session_id()),
           "server identity bound to connection");

    storage::RelayStore store(directory / "identity.db");
    const auto id = network_id(5);
    relay::NetworkService service(store, [id] { return id; });
    relay::NetworkControlChannel channel(service, [] { return 5678; });
    const auto created = channel.handle_authenticated(
        owner_server.device_id(),
        request(protocol::MessageType::network_create_request,
                10,
                protocol::encode_network_create_request({"Authenticated LAN"})));
    expect_operation(created,
                     10,
                     protocol::NetworkOperation::create,
                     protocol::NetworkResultCode::success,
                     id,
                     "authenticated owner creates network");

    expect_error([&] {
        static_cast<void>(channel.note_authenticated(
            guest_identity.device_id(), owner_server.session_id(),
            owner_server.signed_device_key()));
    }, "cannot publish another device's signed encryption key");
    const auto published = channel.note_authenticated(
        owner_server.device_id(), owner_server.session_id(),
        owner_server.signed_device_key());
    expect(published.size() == 1 &&
               published.front().recipient_device_id == owner_server.device_id(),
           "signed device key publication refreshes network membership");
    expect(channel.note_disconnected(owner_server.device_id(), auth::SessionId{}).empty(),
           "unrelated session cannot revoke live device key");
    const auto removed = channel.note_disconnected(owner_server.device_id(),
                                                    owner_server.session_id());
    expect(removed.size() == 1 &&
               removed.front().recipient_device_id == owner_server.device_id(),
           "disconnect removes signed key from shared snapshots");

    const auto selection = protocol::encode_network_member_request(
        {id, guest_identity.device_id()});
    const auto denied = channel.handle_authenticated(
        guest_identity.device_id(),
        request(protocol::MessageType::network_invite_request, 11, selection));
    expect_operation(denied,
                     11,
                     protocol::NetworkOperation::invite,
                     protocol::NetworkResultCode::permission_denied,
                     id,
                     "another identity cannot act as owner");

    const auto encoded = protocol::encode_frame(
        request(protocol::MessageType::network_list_request, 12, {}));
    protocol::FrameStreamDecoder decoder;
    const auto prefix = decoder.push(std::span<const std::byte>(encoded).first(5));
    expect(prefix.empty(), "fragmented control header buffered");
    const auto decoded = decoder.push(std::span<const std::byte>(encoded).subspan(5));
    expect(decoded.size() == 1 && decoded.front().request_id == 12,
           "fragmented control frame assembled");
}

void test_network_key_publication(const std::filesystem::path& directory) {
    using namespace lanlink;

    const auto owner = auth::DeviceIdentity::load_or_create(directory / "publish-owner.key");
    const auto member = auth::DeviceIdentity::load_or_create(directory / "publish-member.key");
    const auto token = auth::AuthToken::from_secret(
        "lanlink-key-publication-authorization-secret");
    auth::ClientHandshake owner_client(owner, token);
    auth::ClientHandshake member_client(member, token);
    auth::ServerHandshake owner_server(
        token, 301, auth::ServerHandshake::Clock::now() + std::chrono::minutes{1});
    auth::ServerHandshake member_server(
        token, 302, auth::ServerHandshake::Clock::now() + std::chrono::minutes{1});
    const auto authenticate = [](auth::ClientHandshake& client,
                                 auth::ServerHandshake& server) {
        const auto challenge = server.handle_hello(client.begin(1));
        const auto result = server.handle_proof(client.handle_challenge(challenge));
        return client.handle_result(result) && server.authenticated();
    };
    expect(authenticate(owner_client, owner_server) &&
               authenticate(member_client, member_server),
           "publication participants authenticate signed encryption keys");

    storage::RelayStore store(directory / "key-publication.db");
    const auto id = network_id(23);
    relay::NetworkService service(store, [id] { return id; });
    relay::NetworkControlChannel channel(service, [] { return 1234; });
    static_cast<void>(channel.note_authenticated(
        owner.device_id(), owner_server.session_id(), owner_server.signed_device_key()));
    static_cast<void>(channel.note_authenticated(
        member.device_id(), member_server.session_id(), member_server.signed_device_key()));
    const auto create = channel.handle_authenticated(
        owner.device_id(), request(protocol::MessageType::network_create_request, 31,
            protocol::encode_network_create_request({"Key test"})));
    expect_operation(create, 31, protocol::NetworkOperation::create,
                     protocol::NetworkResultCode::success, id,
                     "owner creates network for key publication");
    const auto selection = protocol::encode_network_selection_request({id});
    const auto join = channel.handle_authenticated(
        member.device_id(),
        request(protocol::MessageType::network_join_request, 32, selection));
    expect_operation(join, 32, protocol::NetworkOperation::join,
                     protocol::NetworkResultCode::pending_approval, id,
                     "member requests access before receiving network key");
    const auto approved = channel.handle_authenticated(
        owner.device_id(), request(protocol::MessageType::network_approve_request, 33,
            protocol::encode_network_member_request({id, member.device_id()})));
    expect_operation(approved, 33, protocol::NetworkOperation::approve,
                     protocol::NetworkResultCode::success, id,
                     "owner approves key recipient");
    const auto owner_peers = channel.handle_authenticated(
        owner.device_id(),
        request(protocol::MessageType::network_peer_state_request, 34, selection));
    const auto peer_state =
        protocol::decode_network_peer_state(owner_peers.response.payload);
    expect(peer_state.key_epoch == 2 && peer_state.owner_device_id == owner.device_id(),
           "approved membership advances published key epoch");

    const auto secret = auth::NetworkKey::random();
    const auto envelope = auth::seal_network_key(
        owner, owner_client.encryption_key(), member_server.signed_device_key(),
        member.device_id(), id, peer_state.key_epoch, secret);
    const auto publish = request(protocol::MessageType::network_key_publish_request, 0,
        protocol::encode_network_key_publish_request({{envelope}}));
    const auto denied = channel.handle_authenticated(member.device_id(), publish);
    expect_operation(denied, 0, protocol::NetworkOperation::key_publish,
                     protocol::NetworkResultCode::permission_denied, id,
                     "network member cannot publish owner's key");
    expect(denied.events.empty(), "denied key publication sends no envelope");

    auto tampered = envelope;
    tampered.signature.front() ^= std::byte{1};
    const auto invalid = channel.handle_authenticated(owner.device_id(),
        request(protocol::MessageType::network_key_publish_request, 0,
            protocol::encode_network_key_publish_request({{tampered}})));
    expect_operation(invalid, 0, protocol::NetworkOperation::key_publish,
                     protocol::NetworkResultCode::invalid_request, id,
                     "relay rejects unsigned network key publication");
    expect(invalid.events.empty(), "invalid key publication sends no envelope");

    const auto published = channel.handle_authenticated(owner.device_id(), publish);
    expect_operation(published, 0, protocol::NetworkOperation::key_publish,
                     protocol::NetworkResultCode::success, id,
                     "relay accepts signed key for current member and epoch");
    expect(published.events.size() == 1 &&
               published.events.front().recipient_device_id == member.device_id() &&
               published.events.front().frame.type ==
                   protocol::MessageType::network_key_envelope &&
               published.events.front().frame.request_id == 0,
           "relay forwards encrypted key exclusively to intended member");
    if (published.events.size() == 1) {
        const auto received = protocol::decode_network_key_envelope(
            published.events.front().frame.payload);
        expect(auth::open_network_key(member_client.encryption_key(), received,
                                      owner_server.signed_device_key(), member.device_id()) ==
                   secret,
               "recipient decrypts key after relay forwarding");
    }

    const auto left = channel.handle_authenticated(
        member.device_id(),
        request(protocol::MessageType::network_leave_request, 35, selection));
    expect_operation(left, 35, protocol::NetworkOperation::leave,
                     protocol::NetworkResultCode::success, id,
                     "member leaves before old key is republished");
    const auto stale = channel.handle_authenticated(owner.device_id(), publish);
    expect_operation(stale, 0, protocol::NetworkOperation::key_publish,
                     protocol::NetworkResultCode::conflict, id,
                     "relay refuses stale pre-leave key epoch");
    expect(stale.events.empty(), "stale key publication sends no envelope");
    const auto after_leave = auth::seal_network_key(
        owner, owner_client.encryption_key(), member_server.signed_device_key(),
        member.device_id(), id, peer_state.key_epoch + 1, secret);
    const auto removed = channel.handle_authenticated(owner.device_id(),
        request(protocol::MessageType::network_key_publish_request, 0,
            protocol::encode_network_key_publish_request({{after_leave}})));
    expect_operation(removed, 0, protocol::NetworkOperation::key_publish,
                     protocol::NetworkResultCode::target_not_found, id,
                     "relay refuses key to former network member even with current epoch");
    expect(removed.events.empty(), "former member receives no encrypted key");
}

}

int main() {
    const auto directory = make_test_directory();

    try {
        test_management_flow(directory);
        test_invalid_frames(directory);
        test_authenticated_identity(directory);
        test_network_key_publication(directory);
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

    std::cout << "all network control channel tests passed\n";
    return 0;
}
