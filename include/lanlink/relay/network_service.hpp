#pragma once

#include "lanlink/protocol/network_messages.hpp"
#include "lanlink/storage/relay_store.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace lanlink::relay {

struct RoutedNetworkEvent {
    auth::DeviceId recipient_device_id{};
    protocol::NetworkEvent event;

    bool operator==(const RoutedNetworkEvent&) const = default;
};

struct NetworkOperationOutcome {
    protocol::NetworkOperationResult result;
    std::vector<RoutedNetworkEvent> events;

    bool operator==(const NetworkOperationOutcome&) const = default;
};

struct NetworkListOutcome {
    protocol::NetworkResultCode code = protocol::NetworkResultCode::success;
    protocol::NetworkListResult result;

    bool operator==(const NetworkListOutcome&) const = default;
};

class NetworkService {
public:
    using NetworkIdGenerator = std::function<storage::NetworkId()>;

    explicit NetworkService(storage::RelayStore& store,
                            NetworkIdGenerator network_id_generator = {});

    NetworkService(const NetworkService&) = delete;
    NetworkService& operator=(const NetworkService&) = delete;
    NetworkService(NetworkService&&) = delete;
    NetworkService& operator=(NetworkService&&) = delete;

    void record_authenticated_device(const auth::DeviceId& actor, std::int64_t now_ms);

    [[nodiscard]] NetworkOperationOutcome create_network(
        const auth::DeviceId& actor,
        const protocol::NetworkCreateRequest& request,
        std::int64_t now_ms);
    [[nodiscard]] NetworkListOutcome list_networks(
        const auth::DeviceId& actor,
        const protocol::NetworkListRequest& request,
        std::int64_t now_ms);
    [[nodiscard]] NetworkOperationOutcome join_network(
        const auth::DeviceId& actor,
        const protocol::NetworkSelectionRequest& request,
        std::int64_t now_ms);
    [[nodiscard]] NetworkOperationOutcome leave_network(
        const auth::DeviceId& actor,
        const protocol::NetworkSelectionRequest& request,
        std::int64_t now_ms);
    [[nodiscard]] NetworkOperationOutcome invite_member(
        const auth::DeviceId& actor,
        const protocol::NetworkMemberRequest& request,
        std::int64_t now_ms);
    [[nodiscard]] NetworkOperationOutcome approve_member(
        const auth::DeviceId& actor,
        const protocol::NetworkMemberRequest& request,
        std::int64_t now_ms);
    [[nodiscard]] NetworkOperationOutcome kick_member(
        const auth::DeviceId& actor,
        const protocol::NetworkMemberRequest& request,
        std::int64_t now_ms);

private:
    storage::RelayStore& store_;
    NetworkIdGenerator network_id_generator_;
    std::mutex mutex_;
};

}
