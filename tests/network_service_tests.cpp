#include "lanlink/relay/network_service.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
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
                      ("lanlink-network-service-tests-" + std::to_string(suffix));
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

void expect_operation(const lanlink::relay::NetworkOperationOutcome& outcome,
                      const lanlink::protocol::NetworkOperation operation,
                      const lanlink::protocol::NetworkResultCode code,
                      const lanlink::protocol::NetworkId& id,
                      const std::string_view name) {
    expect(outcome.result == lanlink::protocol::NetworkOperationResult{operation, code, id},
           name);

    try {
        static_cast<void>(lanlink::protocol::encode_network_operation_result(outcome.result));
    } catch (const std::exception&) {
        expect(false, std::string(name) + " encodable");
    }
}

void expect_event(const lanlink::relay::RoutedNetworkEvent& routed,
                  const lanlink::auth::DeviceId& recipient,
                  const lanlink::protocol::NetworkEventKind kind,
                  const lanlink::protocol::NetworkId& id,
                  const lanlink::auth::DeviceId& subject,
                  const std::string_view name) {
    expect(routed == lanlink::relay::RoutedNetworkEvent{
                         recipient, {kind, id, subject}},
           name);

    try {
        static_cast<void>(lanlink::protocol::encode_network_event(routed.event));
    } catch (const std::exception&) {
        expect(false, std::string(name) + " encodable");
    }
}

void test_create_and_list(const std::filesystem::path& directory) {
    using namespace lanlink;

    storage::RelayStore store(directory / "create-list.db");
    const auto actor = device_id(1);
    const auto existing_owner = device_id(2);
    const auto existing_id = network_id(1);
    const auto created_id = network_id(2);
    const auto second_id = network_id(3);
    store.record_device(existing_owner, 10);
    store.create_network({existing_id, "Existing LAN", existing_owner, 10});
    std::vector<storage::NetworkId> generated{{}, existing_id, created_id, second_id};
    std::size_t generated_index = 0;
    relay::NetworkService service(store, [&] {
        return generated.at(generated_index++);
    });

    const auto created = service.create_network(actor, {"Panda LAN"}, 100);
    expect_operation(created,
                     protocol::NetworkOperation::create,
                     protocol::NetworkResultCode::success,
                     created_id,
                     "create succeeds after id retries");
    expect(created.events.empty(), "create has no events");
    expect(store.find_network(created_id) ==
               storage::NetworkRecord{created_id, "Panda LAN", actor, 100},
           "created network persisted");

    const auto second = service.create_network(actor, {"Second LAN"}, 200);
    expect_operation(second,
                     protocol::NetworkOperation::create,
                     protocol::NetworkResultCode::success,
                     second_id,
                     "second create succeeds");

    const auto invalid = service.create_network(actor, {""}, 300);
    expect_operation(invalid,
                     protocol::NetworkOperation::create,
                     protocol::NetworkResultCode::invalid_request,
                     {},
                     "invalid create rejected");
    std::string invalid_utf8;
    invalid_utf8.push_back(static_cast<char>(0xc0));
    invalid_utf8.push_back(static_cast<char>(0xaf));
    const auto invalid_encoding = service.create_network(actor, {invalid_utf8}, 300);
    expect_operation(invalid_encoding,
                     protocol::NetworkOperation::create,
                     protocol::NetworkResultCode::invalid_request,
                     {},
                     "invalid create encoding rejected");

    const auto listed = service.list_networks(actor, {}, 400);
    expect(listed.code == protocol::NetworkResultCode::success, "list succeeds");
    expect(listed.result.networks.size() == 2, "list returns actor networks");
    expect(listed.result.networks.size() == 2 &&
               listed.result.networks[0] == protocol::NetworkSummary{
                                                created_id,
                                                actor,
                                                protocol::NetworkRole::owner,
                                                100,
                                                1,
                                                "Panda LAN"},
           "first owner summary");
    expect(listed.result.networks.size() == 2 &&
               listed.result.networks[1] == protocol::NetworkSummary{
                                                second_id,
                                                actor,
                                                protocol::NetworkRole::owner,
                                                200,
                                                1,
                                                "Second LAN"},
           "second owner summary");

    try {
        static_cast<void>(protocol::encode_network_list_result(listed.result));
    } catch (const std::exception&) {
        expect(false, "list result encodable");
    }

    const auto stored_actor = store.find_device(actor);
    expect(stored_actor && stored_actor->last_seen_at_ms == 400,
           "list updates authenticated device timestamp");

    const auto empty_actor = device_id(3);
    const auto empty = service.list_networks(empty_actor, {}, 500);
    expect(empty.code == protocol::NetworkResultCode::success &&
               empty.result.networks.empty(),
           "unknown authenticated device gets empty list");

    relay::NetworkService broken(store, []() -> storage::NetworkId {
        throw std::runtime_error("generator unavailable");
    });
    const auto internal = broken.create_network(actor, {"Broken LAN"}, 600);
    expect_operation(internal,
                     protocol::NetworkOperation::create,
                     protocol::NetworkResultCode::internal_error,
                     {},
                     "generator failure mapped to internal error");

    relay::NetworkService exhausted(store, [] {
        return storage::NetworkId{};
    });
    const auto conflict = exhausted.create_network(actor, {"No Identifier"}, 700);
    expect_operation(conflict,
                     protocol::NetworkOperation::create,
                     protocol::NetworkResultCode::conflict,
                     {},
                     "identifier exhaustion mapped to conflict");

    auth::DeviceId zero_actor{};
    expect_error([&] {
        static_cast<void>(service.create_network(zero_actor, {"Invalid"}, 800));
    }, "zero actor rejected");
    expect_error([&] {
        static_cast<void>(service.list_networks(actor, {}, -1));
    }, "negative operation time rejected");
}

void test_invitation_join_and_leave(const std::filesystem::path& directory) {
    using namespace lanlink;

    storage::RelayStore store(directory / "invite-join-leave.db");
    const auto owner = device_id(10);
    const auto invited = device_id(11);
    const auto outsider = device_id(12);
    const auto missing = device_id(13);
    const auto id = network_id(10);
    relay::NetworkService service(store, [id] {
        return id;
    });
    expect_operation(service.create_network(owner, {"Invite LAN"}, 1'000),
                     protocol::NetworkOperation::create,
                     protocol::NetworkResultCode::success,
                     id,
                     "invite test network created");
    static_cast<void>(service.list_networks(invited, {}, 1'010));
    static_cast<void>(service.list_networks(outsider, {}, 1'020));

    const protocol::NetworkMemberRequest invite{id, invited};
    const auto denied = service.invite_member(outsider, invite, 1'100);
    expect_operation(denied,
                     protocol::NetworkOperation::invite,
                     protocol::NetworkResultCode::permission_denied,
                     id,
                     "non-owner invite denied");

    const auto unknown = service.invite_member(owner, {id, missing}, 1'110);
    expect_operation(unknown,
                     protocol::NetworkOperation::invite,
                     protocol::NetworkResultCode::target_not_found,
                     id,
                     "unknown invite target rejected");

    const auto invited_outcome = service.invite_member(owner, invite, 1'120);
    expect_operation(invited_outcome,
                     protocol::NetworkOperation::invite,
                     protocol::NetworkResultCode::success,
                     id,
                     "owner invitation succeeds");
    expect(invited_outcome.events.size() == 1, "invitation emits one event");
    if (invited_outcome.events.size() == 1) {
        expect_event(invited_outcome.events.front(),
                     invited,
                     protocol::NetworkEventKind::invited,
                     id,
                     invited,
                     "invitation routed to target");
    }

    const auto duplicate = service.invite_member(owner, invite, 1'130);
    expect_operation(duplicate,
                     protocol::NetworkOperation::invite,
                     protocol::NetworkResultCode::already_exists,
                     id,
                     "duplicate invitation rejected");
    expect(duplicate.events.empty(), "duplicate invitation emits no event");

    const auto joined = service.join_network(invited, {id}, 1'200);
    expect_operation(joined,
                     protocol::NetworkOperation::join,
                     protocol::NetworkResultCode::success,
                     id,
                     "invited device joins");
    expect(joined.events.size() == 1, "invited join emits owner event");
    if (joined.events.size() == 1) {
        expect_event(joined.events.front(),
                     owner,
                     protocol::NetworkEventKind::member_joined,
                     id,
                     invited,
                     "member joined routed to owner");
    }
    expect(!store.find_invitation(id, invited), "join consumes invitation");

    const auto joined_again = service.join_network(invited, {id}, 1'210);
    expect_operation(joined_again,
                     protocol::NetworkOperation::join,
                     protocol::NetworkResultCode::already_exists,
                     id,
                     "member cannot join twice");

    const auto member_list = service.list_networks(invited, {}, 1'220);
    expect(member_list.code == protocol::NetworkResultCode::success &&
               member_list.result.networks.size() == 1 &&
               member_list.result.networks.front().role == protocol::NetworkRole::member &&
               member_list.result.networks.front().member_count == 2,
           "member list reports role and count");

    const auto owner_leave = service.leave_network(owner, {id}, 1'300);
    expect_operation(owner_leave,
                     protocol::NetworkOperation::leave,
                     protocol::NetworkResultCode::permission_denied,
                     id,
                     "owner cannot leave owned network");
    const auto outsider_leave = service.leave_network(outsider, {id}, 1'310);
    expect_operation(outsider_leave,
                     protocol::NetworkOperation::leave,
                     protocol::NetworkResultCode::not_member,
                     id,
                     "outsider cannot leave network");

    const auto left = service.leave_network(invited, {id}, 1'320);
    expect_operation(left,
                     protocol::NetworkOperation::leave,
                     protocol::NetworkResultCode::success,
                     id,
                     "member leaves network");
    expect(left.events.size() == 1, "leave emits owner event");
    if (left.events.size() == 1) {
        expect_event(left.events.front(),
                     owner,
                     protocol::NetworkEventKind::member_left,
                     id,
                     invited,
                     "member left routed to owner");
    }

    const auto left_again = service.leave_network(invited, {id}, 1'330);
    expect_operation(left_again,
                     protocol::NetworkOperation::leave,
                     protocol::NetworkResultCode::not_member,
                     id,
                     "member cannot leave twice");
}

void test_approval_and_kick(const std::filesystem::path& directory) {
    using namespace lanlink;

    storage::RelayStore store(directory / "approve-kick.db");
    const auto owner = device_id(20);
    const auto requester = device_id(21);
    const auto observer = device_id(22);
    const auto unknown = device_id(23);
    const auto id = network_id(20);
    relay::NetworkService service(store, [id] {
        return id;
    });
    static_cast<void>(service.create_network(owner, {"Approval LAN"}, 2'000));
    static_cast<void>(service.list_networks(requester, {}, 2'010));
    static_cast<void>(service.list_networks(observer, {}, 2'020));

    const auto requested = service.join_network(requester, {id}, 2'100);
    expect_operation(requested,
                     protocol::NetworkOperation::join,
                     protocol::NetworkResultCode::pending_approval,
                     id,
                     "join without invitation waits for approval");
    expect(requested.events.size() == 1, "join request emits owner event");
    if (requested.events.size() == 1) {
        expect_event(requested.events.front(),
                     owner,
                     protocol::NetworkEventKind::join_requested,
                     id,
                     requester,
                     "join request routed to owner");
    }

    const auto requested_again = service.join_network(requester, {id}, 2'110);
    expect_operation(requested_again,
                     protocol::NetworkOperation::join,
                     protocol::NetworkResultCode::pending_approval,
                     id,
                     "duplicate join remains pending");
    expect(requested_again.events.empty(), "duplicate join emits no event");

    const auto conflicting_invite =
        service.invite_member(owner, {id, requester}, 2'120);
    expect_operation(conflicting_invite,
                     protocol::NetworkOperation::invite,
                     protocol::NetworkResultCode::conflict,
                     id,
                     "pending join conflicts with invitation");

    const auto denied = service.approve_member(observer, {id, requester}, 2'130);
    expect_operation(denied,
                     protocol::NetworkOperation::approve,
                     protocol::NetworkResultCode::permission_denied,
                     id,
                     "non-owner approval denied");
    const auto unknown_target = service.approve_member(owner, {id, unknown}, 2'140);
    expect_operation(unknown_target,
                     protocol::NetworkOperation::approve,
                     protocol::NetworkResultCode::target_not_found,
                     id,
                     "unknown approval target rejected");
    const auto no_request = service.approve_member(owner, {id, observer}, 2'150);
    expect_operation(no_request,
                     protocol::NetworkOperation::approve,
                     protocol::NetworkResultCode::not_found,
                     id,
                     "missing join request rejected");

    const auto approved = service.approve_member(owner, {id, requester}, 2'200);
    expect_operation(approved,
                     protocol::NetworkOperation::approve,
                     protocol::NetworkResultCode::success,
                     id,
                     "owner approves join request");
    expect(approved.events.size() == 1, "approval notifies new member");
    if (approved.events.size() == 1) {
        expect_event(approved.events.front(),
                     requester,
                     protocol::NetworkEventKind::member_joined,
                     id,
                     requester,
                     "approval event routed to new member");
    }
    expect(!store.find_join_request(id, requester), "approval consumes join request");

    const auto approved_again = service.approve_member(owner, {id, requester}, 2'210);
    expect_operation(approved_again,
                     protocol::NetworkOperation::approve,
                     protocol::NetworkResultCode::already_exists,
                     id,
                     "approved member cannot be approved twice");

    static_cast<void>(service.invite_member(owner, {id, observer}, 2'220));
    static_cast<void>(service.join_network(observer, {id}, 2'230));

    const auto member_kick = service.kick_member(requester, {id, observer}, 2'300);
    expect_operation(member_kick,
                     protocol::NetworkOperation::kick,
                     protocol::NetworkResultCode::permission_denied,
                     id,
                     "non-owner kick denied");
    const auto owner_kick = service.kick_member(owner, {id, owner}, 2'310);
    expect_operation(owner_kick,
                     protocol::NetworkOperation::kick,
                     protocol::NetworkResultCode::permission_denied,
                     id,
                     "owner cannot kick self");
    const auto unknown_kick = service.kick_member(owner, {id, unknown}, 2'320);
    expect_operation(unknown_kick,
                     protocol::NetworkOperation::kick,
                     protocol::NetworkResultCode::target_not_found,
                     id,
                     "unknown kick target rejected");

    const auto kicked = service.kick_member(owner, {id, requester}, 2'330);
    expect_operation(kicked,
                     protocol::NetworkOperation::kick,
                     protocol::NetworkResultCode::success,
                     id,
                     "owner kicks member");
    expect(kicked.events.size() == 2, "kick notifies target and remaining peer");
    if (kicked.events.size() == 2) {
        expect_event(kicked.events[0],
                     requester,
                     protocol::NetworkEventKind::member_kicked,
                     id,
                     requester,
                     "kick event routed to target");
        expect_event(kicked.events[1],
                     observer,
                     protocol::NetworkEventKind::member_kicked,
                     id,
                     requester,
                     "kick event routed to remaining peer");
    }

    const auto kicked_again = service.kick_member(owner, {id, requester}, 2'340);
    expect_operation(kicked_again,
                     protocol::NetworkOperation::kick,
                     protocol::NetworkResultCode::not_member,
                     id,
                     "member cannot be kicked twice");
    const auto requester_list = service.list_networks(requester, {}, 2'350);
    expect(requester_list.code == protocol::NetworkResultCode::success &&
               requester_list.result.networks.empty(),
           "kicked member no longer lists network");
}

void test_restart_and_concurrency(const std::filesystem::path& directory) {
    using namespace lanlink;

    const auto path = directory / "restart-concurrency.db";
    const auto owner = device_id(30);
    const auto requester = device_id(31);
    const auto concurrent = device_id(32);
    const auto id = network_id(30);

    {
        storage::RelayStore store(path);
        relay::NetworkService service(store, [id] {
            return id;
        });
        static_cast<void>(service.create_network(owner, {"Restart LAN"}, 3'000));
        const auto pending = service.join_network(requester, {id}, 3'100);
        expect_operation(pending,
                         protocol::NetworkOperation::join,
                         protocol::NetworkResultCode::pending_approval,
                         id,
                         "join request stored before restart");
    }

    {
        storage::RelayStore store(path);
        relay::NetworkService service(store);
        const auto approved = service.approve_member(owner, {id, requester}, 3'200);
        expect_operation(approved,
                         protocol::NetworkOperation::approve,
                         protocol::NetworkResultCode::success,
                         id,
                         "persisted join request approved after restart");

        constexpr int thread_count = 8;
        std::atomic_int pending_count = 0;
        std::atomic_int request_event_count = 0;
        std::atomic_int unexpected_count = 0;
        std::vector<std::thread> threads;

        for (int index = 0; index < thread_count; ++index) {
            threads.emplace_back([&, index] {
                const auto outcome = service.join_network(concurrent, {id}, 3'300 + index);

                if (outcome.result.code == protocol::NetworkResultCode::pending_approval) {
                    ++pending_count;
                    request_event_count += static_cast<int>(outcome.events.size());
                } else {
                    ++unexpected_count;
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        expect(pending_count.load() == thread_count, "concurrent joins remain pending");
        expect(request_event_count.load() == 1, "concurrent joins emit one request event");
        expect(unexpected_count.load() == 0, "concurrent joins have no unexpected result");
        expect(store.list_join_requests_for_network(id).size() == 1,
               "concurrent joins store one request");

        std::atomic_int success_count = 0;
        std::atomic_int exists_count = 0;
        unexpected_count.store(0);
        threads.clear();

        for (int index = 0; index < thread_count; ++index) {
            threads.emplace_back([&, index] {
                const auto outcome =
                    service.approve_member(owner, {id, concurrent}, 3'400 + index);

                if (outcome.result.code == protocol::NetworkResultCode::success) {
                    ++success_count;
                } else if (outcome.result.code ==
                           protocol::NetworkResultCode::already_exists) {
                    ++exists_count;
                } else {
                    ++unexpected_count;
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        expect(success_count.load() == 1, "one concurrent approval succeeds");
        expect(exists_count.load() == thread_count - 1,
               "remaining concurrent approvals see membership");
        expect(unexpected_count.load() == 0,
               "concurrent approvals have no unexpected result");
        expect(!store.find_join_request(id, concurrent),
               "concurrent approval consumes request");
    }
}

void test_missing_and_invalid_requests(const std::filesystem::path& directory) {
    using namespace lanlink;

    storage::RelayStore store(directory / "invalid.db");
    const auto actor = device_id(40);
    const auto target = device_id(41);
    const auto missing_id = network_id(40);
    relay::NetworkService service(store);
    static_cast<void>(service.list_networks(target, {}, 4'000));

    expect_operation(service.join_network(actor, {missing_id}, 4'100),
                     protocol::NetworkOperation::join,
                     protocol::NetworkResultCode::not_found,
                     missing_id,
                     "missing join network rejected");
    expect_operation(service.leave_network(actor, {missing_id}, 4'110),
                     protocol::NetworkOperation::leave,
                     protocol::NetworkResultCode::not_found,
                     missing_id,
                     "missing leave network rejected");
    expect_operation(service.invite_member(actor, {missing_id, target}, 4'120),
                     protocol::NetworkOperation::invite,
                     protocol::NetworkResultCode::not_found,
                     missing_id,
                     "missing invite network rejected");
    expect_operation(service.approve_member(actor, {missing_id, target}, 4'130),
                     protocol::NetworkOperation::approve,
                     protocol::NetworkResultCode::not_found,
                     missing_id,
                     "missing approval network rejected");
    expect_operation(service.kick_member(actor, {missing_id, target}, 4'140),
                     protocol::NetworkOperation::kick,
                     protocol::NetworkResultCode::not_found,
                     missing_id,
                     "missing kick network rejected");

    protocol::NetworkDeviceId zero_target{};
    expect_operation(service.invite_member(actor, {missing_id, zero_target}, 4'200),
                     protocol::NetworkOperation::invite,
                     protocol::NetworkResultCode::invalid_request,
                     missing_id,
                     "zero invite target rejected");
    expect_operation(service.approve_member(actor, {missing_id, zero_target}, 4'210),
                     protocol::NetworkOperation::approve,
                     protocol::NetworkResultCode::invalid_request,
                     missing_id,
                     "zero approval target rejected");
    expect_operation(service.kick_member(actor, {missing_id, zero_target}, 4'220),
                     protocol::NetworkOperation::kick,
                     protocol::NetworkResultCode::invalid_request,
                     missing_id,
                     "zero kick target rejected");

    protocol::NetworkId zero_network{};
    expect_error([&] {
        static_cast<void>(service.join_network(actor, {zero_network}, 4'300));
    }, "zero join network rejected");
    expect_error([&] {
        static_cast<void>(service.leave_network(actor, {zero_network}, 4'310));
    }, "zero leave network rejected");
    expect_error([&] {
        static_cast<void>(service.invite_member(actor, {zero_network, target}, 4'320));
    }, "zero invite network rejected");
    expect_error([&] {
        static_cast<void>(service.join_network(actor, {missing_id}, -1));
    }, "negative join time rejected");
}

}

int main() {
    const auto directory = make_test_directory();

    try {
        test_create_and_list(directory);
        test_invitation_join_and_leave(directory);
        test_approval_and_kick(directory);
        test_restart_and_concurrency(directory);
        test_missing_and_invalid_requests(directory);
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

    std::cout << "all network service tests passed\n";
    return 0;
}
