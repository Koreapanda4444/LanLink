#include "lanlink/auth/identity.hpp"
#include "lanlink/core/logger.hpp"
#include "lanlink/relay/network_control_channel.hpp"
#include "lanlink/relay/network_service.hpp"
#include "lanlink/storage/relay_store.hpp"
#include "lanlink/transport/quic.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace auth = lanlink::auth;
namespace core = lanlink::core;
namespace protocol = lanlink::protocol;
namespace relay = lanlink::relay;
namespace storage = lanlink::storage;
namespace transport = lanlink::transport;

void expect(const bool condition, const std::string& description) {
    if (!condition) {
        throw std::runtime_error(description);
    }
}

template <typename Predicate>
void wait_until(Predicate&& predicate, const std::string& description) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(description);
        }
        std::this_thread::sleep_for(10ms);
    }
}

template <typename Function>
void expect_error(Function&& function, const std::string& description) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(description);
}

void test_network_management(const std::filesystem::path& directory) {
    const auto token = directory / "auth.token";
    const auto database = directory / "relay.db";
    const auto owner_identity = auth::DeviceIdentity::load_or_create(directory / "owner.identity");
    const auto member_identity = auth::DeviceIdentity::load_or_create(directory / "member.identity");
    core::Logger relay_logger(directory / "relay.log", "relay-test", core::LogLevel::info,
                              1024 * 1024, 2);
    core::Logger owner_logger(directory / "owner.log", "owner-test", core::LogLevel::info,
                              1024 * 1024, 2);
    core::Logger member_logger(directory / "member.log", "member-test", core::LogLevel::info,
                               1024 * 1024, 2);
    storage::RelayStore store(database);
    relay::NetworkService network_service(store);
    relay::NetworkControlChannel channel(network_service);

    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto port = static_cast<std::uint16_t>(35'000 + (ticks % 20'000));
    transport::QuicServerOptions server_options;
    server_options.listen_host = "127.0.0.1";
    server_options.port = port;
    server_options.certificate_file = directory / "cert.pem";
    server_options.private_key_file = directory / "key.pem";
    server_options.auth_token_file = token;
    server_options.handshake_timeout = 3s;
    server_options.authentication_timeout = 3s;

    transport::QuicRelayServer server(std::move(server_options), relay_logger, channel);
    server.start();

    transport::QuicClientOptions owner_options;
    owner_options.relay_host = "127.0.0.1";
    owner_options.port = port;
    owner_options.identity_file = directory / "owner.identity";
    owner_options.auth_token_file = token;
    owner_options.handshake_timeout = 3s;
    owner_options.authentication_timeout = 3s;
    owner_options.reconnect_initial_delay = 100ms;
    owner_options.reconnect_maximum_delay = 250ms;
    auto member_options = owner_options;
    member_options.identity_file = directory / "member.identity";

    transport::QuicRelayClient owner(std::move(owner_options), owner_logger);
    transport::QuicRelayClient member(std::move(member_options), member_logger);

    expect_error([&] { static_cast<void>(owner.list_networks(100ms)); },
                 "request before authentication must fail");
    expect_error([&] { static_cast<void>(owner.list_networks(0ms)); },
                 "non-positive timeout must fail");

    std::atomic<int> join_requests = 0;
    std::atomic<int> member_joined = 0;
    std::atomic<int> invitations = 0;
    std::atomic_bool callback_request_succeeded = false;
    owner.set_network_event_handler([&](const protocol::NetworkEvent& event) {
        if (event.kind == protocol::NetworkEventKind::join_requested) {
            const auto networks = owner.list_networks(2s);
            callback_request_succeeded.store(networks.code ==
                                                  protocol::NetworkResultCode::success &&
                                              networks.result.networks.size() == 1);
            join_requests.fetch_add(1);
        }
    });
    member.set_network_event_handler([&](const protocol::NetworkEvent& event) {
        if (event.kind == protocol::NetworkEventKind::member_joined) {
            member_joined.fetch_add(1);
        }
        if (event.kind == protocol::NetworkEventKind::invited) {
            invitations.fetch_add(1);
        }
    });

    std::exception_ptr owner_error;
    std::exception_ptr member_error;
    std::jthread owner_worker([&](std::stop_token token) {
        try {
            owner.run(token);
        } catch (...) {
            owner_error = std::current_exception();
        }
    });
    std::jthread member_worker([&](std::stop_token token) {
        try {
            member.run(token);
        } catch (...) {
            member_error = std::current_exception();
        }
    });

    wait_until([&] { return owner.authenticated() && member.authenticated(); },
               "both clients must authenticate");
    expect(store.find_device(owner_identity.device_id()).has_value() &&
               store.find_device(member_identity.device_id()).has_value(),
           "relay must persist authenticated client identities");

    const auto create = owner.create_network("Integration LAN");
    const auto id = create.network_id;
    expect(create.operation == protocol::NetworkOperation::create &&
               create.code == protocol::NetworkResultCode::success,
           "create must succeed over authenticated control stream");
    expect(store.find_network(id).has_value(), "create must persist network in SQLite");
    expect_error([&] { static_cast<void>(owner.create_network("")); },
                 "invalid network name must fail locally");

    std::vector<std::future<transport::NetworkListReply>> concurrent;
    for (int index = 0; index < 16; ++index) {
        concurrent.push_back(std::async(std::launch::async, [&] {
            return owner.list_networks();
        }));
    }
    for (auto& response : concurrent) {
        const auto list = response.get();
        expect(list.code == protocol::NetworkResultCode::success &&
                   list.result.networks.size() == 1 &&
                   list.result.networks.front().network_id == id &&
                   list.result.networks.front().name == "Integration LAN",
               "concurrent request ids must correlate to the correct list responses");
    }

    const auto pending = member.join_network(id);
    expect(pending.operation == protocol::NetworkOperation::join &&
               pending.code == protocol::NetworkResultCode::pending_approval,
           "uninvited member join must require approval");
    wait_until([&] { return join_requests.load() == 1; },
               "owner must receive join request event");
    expect(callback_request_succeeded.load(),
           "network event handler must be able to request through relay");
    expect(store.find_join_request(id, member_identity.device_id()).has_value(),
           "join request must persist");

    const auto approved = owner.approve_member(id, member_identity.device_id());
    expect(approved.code == protocol::NetworkResultCode::success,
           "owner must approve member over relay");
    wait_until([&] { return member_joined.load() == 1; },
               "member must receive approval event");
    const auto member_list = member.list_networks();
    expect(member_list.result.networks.size() == 1 &&
               member_list.result.networks.front().role == protocol::NetworkRole::member,
           "member list must include approved network");
    expect(store.list_members(id).size() == 2, "membership must persist in SQLite");

    const auto kicked = owner.kick_member(id, member_identity.device_id());
    expect(kicked.code == protocol::NetworkResultCode::success,
           "owner must kick member over relay");
    expect(member.list_networks().result.networks.empty(),
           "kicked member must no longer list network");

    const auto invited = owner.invite_member(id, member_identity.device_id());
    expect(invited.code == protocol::NetworkResultCode::success,
           "owner must invite known client");
    wait_until([&] { return invitations.load() == 1; },
               "invitee must receive invitation event");
    expect(store.find_invitation(id, member_identity.device_id()).has_value(),
           "invitation must persist in SQLite");
    expect(member.join_network(id).code == protocol::NetworkResultCode::success,
           "invited member must join without pending approval");
    expect(member.leave_network(id).code == protocol::NetworkResultCode::success,
           "member must leave over relay");
    expect(store.list_members(id).size() == 1,
           "leave must remove member from SQLite");

    server.stop();
    wait_until([&] { return !owner.authenticated() && !member.authenticated(); },
               "server stop must invalidate authenticated client state");
    expect_error([&] { static_cast<void>(owner.list_networks(100ms)); },
                 "request after disconnect must fail");
    owner_worker.request_stop();
    member_worker.request_stop();
    owner.stop();
    member.stop();
    owner_worker.join();
    member_worker.join();
    if (owner_error) {
        std::rethrow_exception(owner_error);
    }
    if (member_error) {
        std::rethrow_exception(member_error);
    }

    storage::RelayStore reopened(database);
    expect(reopened.find_network(id).has_value() &&
               reopened.list_members(id).size() == 1,
           "network and owner membership must survive reopening SQLite");
}

}

int main(const int argc, char* argv[]) {
    try {
        if (argc != 2) {
            throw std::invalid_argument("usage: network-client-tests fixture-directory");
        }
        test_network_management(argv[1]);
        std::cout << "network management QUIC integration passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "network management QUIC integration failed: " << error.what() << '\n';
        return 1;
    }
}
