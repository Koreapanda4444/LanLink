#include "lanlink/relay/network_service.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lanlink::relay {
namespace {

constexpr std::size_t network_id_generation_attempts = 32;

template <std::size_t Size>
bool is_zero(const std::array<std::byte, Size>& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
        return byte == std::byte{0};
    });
}

storage::NetworkId generate_network_id() {
    storage::NetworkId result{};
    const int status = RAND_bytes(reinterpret_cast<unsigned char*>(result.data()),
                                  static_cast<int>(result.size()));

    if (status != 1) {
        throw std::runtime_error("network id generation failed");
    }

    return result;
}

void require_actor_and_time(const auth::DeviceId& actor, const std::int64_t now_ms) {
    if (is_zero(actor)) {
        throw std::invalid_argument("network operation requires an authenticated device");
    }
    if (now_ms < 0) {
        throw std::invalid_argument("network operation timestamp must not be negative");
    }
}

void require_selection_request(const protocol::NetworkSelectionRequest& request) {
    static_cast<void>(protocol::encode_network_selection_request(request));
}

bool valid_member_request(const protocol::NetworkMemberRequest& request) {
    require_selection_request({request.network_id});

    try {
        static_cast<void>(protocol::encode_network_member_request(request));
        return true;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

NetworkOperationOutcome operation_outcome(const protocol::NetworkOperation operation,
                                          const protocol::NetworkResultCode code,
                                          const protocol::NetworkId& network_id = {}) {
    return {{operation, code, network_id}, {}};
}

NetworkListOutcome list_outcome(const protocol::NetworkResultCode code) {
    return {code, {}};
}

const storage::MembershipRecord* find_member(
    const std::vector<storage::MembershipRecord>& members,
    const auth::DeviceId& device_id) {
    const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) {
        return member.device_id == device_id;
    });
    return found == members.end() ? nullptr : &*found;
}

RoutedNetworkEvent routed_event(const auth::DeviceId& recipient,
                                const protocol::NetworkEventKind kind,
                                const protocol::NetworkId& network_id,
                                const auth::DeviceId& subject) {
    return {recipient, {kind, network_id, subject}};
}

std::vector<RoutedNetworkEvent> events_for_members(
    const std::vector<storage::MembershipRecord>& members,
    const std::optional<auth::DeviceId>& excluded,
    const protocol::NetworkEventKind kind,
    const protocol::NetworkId& network_id,
    const auth::DeviceId& subject) {
    std::vector<RoutedNetworkEvent> result;
    result.reserve(members.size());

    for (const auto& member : members) {
        if (!excluded || member.device_id != *excluded) {
            result.push_back(routed_event(member.device_id, kind, network_id, subject));
        }
    }

    return result;
}

bool network_limit_reached(storage::RelayStore& store, const auth::DeviceId& device_id) {
    return store.list_networks_for_device(device_id).size() >=
           protocol::max_network_list_entries;
}

protocol::NetworkSummary make_summary(const storage::NetworkRecord& network,
                                      const auth::DeviceId& actor,
                                      const std::vector<storage::MembershipRecord>& members) {
    const auto* membership = find_member(members, actor);

    if (membership == nullptr || members.empty() ||
        members.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("network membership state is inconsistent");
    }

    protocol::NetworkSummary result;
    result.network_id = network.id;
    result.owner_device_id = network.owner_device_id;
    result.role = membership->role == storage::MembershipRole::owner
                      ? protocol::NetworkRole::owner
                      : protocol::NetworkRole::member;
    result.created_at_ms = network.created_at_ms;
    result.member_count = static_cast<std::uint32_t>(members.size());
    result.name = network.name;
    return result;
}

}

NetworkService::NetworkService(storage::RelayStore& store,
                               NetworkIdGenerator network_id_generator)
    : store_(store),
      network_id_generator_(network_id_generator ? std::move(network_id_generator)
                                                 : NetworkIdGenerator{generate_network_id}) {
}

void NetworkService::record_authenticated_device(const auth::DeviceId& actor,
                                                 const std::int64_t now_ms) {
    require_actor_and_time(actor, now_ms);
    std::lock_guard lock(mutex_);
    store_.record_device(actor, now_ms);
}

NetworkOperationOutcome NetworkService::create_network(
    const auth::DeviceId& actor,
    const protocol::NetworkCreateRequest& request,
    const std::int64_t now_ms) {
    require_actor_and_time(actor, now_ms);

    try {
        static_cast<void>(protocol::encode_network_create_request(request));
    } catch (const std::invalid_argument&) {
        return operation_outcome(protocol::NetworkOperation::create,
                                 protocol::NetworkResultCode::invalid_request);
    }

    std::lock_guard lock(mutex_);

    try {
        store_.record_device(actor, now_ms);

        if (network_limit_reached(store_, actor)) {
            return operation_outcome(protocol::NetworkOperation::create,
                                     protocol::NetworkResultCode::limit_reached);
        }

        for (std::size_t attempt = 0; attempt < network_id_generation_attempts; ++attempt) {
            const auto network_id = network_id_generator_();

            if (is_zero(network_id) || store_.find_network(network_id).has_value()) {
                continue;
            }

            try {
                store_.create_network({network_id, request.name, actor, now_ms});
            } catch (const std::exception&) {
                if (store_.find_network(network_id).has_value()) {
                    continue;
                }

                throw;
            }

            return operation_outcome(protocol::NetworkOperation::create,
                                     protocol::NetworkResultCode::success,
                                     network_id);
        }

        return operation_outcome(protocol::NetworkOperation::create,
                                 protocol::NetworkResultCode::conflict);
    } catch (const std::length_error&) {
        return operation_outcome(protocol::NetworkOperation::create,
                                 protocol::NetworkResultCode::limit_reached);
    } catch (const std::exception&) {
        return operation_outcome(protocol::NetworkOperation::create,
                                 protocol::NetworkResultCode::internal_error);
    }
}

NetworkListOutcome NetworkService::list_networks(
    const auth::DeviceId& actor,
    const protocol::NetworkListRequest& request,
    const std::int64_t now_ms) {
    require_actor_and_time(actor, now_ms);
    static_cast<void>(protocol::encode_network_list_request(request));
    std::lock_guard lock(mutex_);

    try {
        store_.record_device(actor, now_ms);
        const auto networks = store_.list_networks_for_device(actor);

        if (networks.size() > protocol::max_network_list_entries) {
            return list_outcome(protocol::NetworkResultCode::limit_reached);
        }

        NetworkListOutcome outcome;
        outcome.result.networks.reserve(networks.size());

        for (const auto& network : networks) {
            outcome.result.networks.push_back(
                make_summary(network, actor, store_.list_members(network.id)));
        }

        return outcome;
    } catch (const std::exception&) {
        return list_outcome(protocol::NetworkResultCode::internal_error);
    }
}

NetworkOperationOutcome NetworkService::join_network(
    const auth::DeviceId& actor,
    const protocol::NetworkSelectionRequest& request,
    const std::int64_t now_ms) {
    require_actor_and_time(actor, now_ms);
    require_selection_request(request);
    std::lock_guard lock(mutex_);

    try {
        store_.record_device(actor, now_ms);
        const auto network = store_.find_network(request.network_id);

        if (!network) {
            return operation_outcome(protocol::NetworkOperation::join,
                                     protocol::NetworkResultCode::not_found,
                                     request.network_id);
        }

        const auto members = store_.list_members(request.network_id);

        if (find_member(members, actor) != nullptr) {
            return operation_outcome(protocol::NetworkOperation::join,
                                     protocol::NetworkResultCode::already_exists,
                                     request.network_id);
        }
        if (network_limit_reached(store_, actor)) {
            return operation_outcome(protocol::NetworkOperation::join,
                                     protocol::NetworkResultCode::limit_reached,
                                     request.network_id);
        }

        if (store_.find_invitation(request.network_id, actor)) {
            if (!store_.add_member(request.network_id, actor, now_ms)) {
                return operation_outcome(protocol::NetworkOperation::join,
                                         protocol::NetworkResultCode::conflict,
                                         request.network_id);
            }

            auto outcome = operation_outcome(protocol::NetworkOperation::join,
                                             protocol::NetworkResultCode::success,
                                             request.network_id);
            outcome.events = events_for_members(store_.list_members(request.network_id),
                                                actor,
                                                protocol::NetworkEventKind::member_joined,
                                                request.network_id,
                                                actor);
            return outcome;
        }

        if (store_.find_join_request(request.network_id, actor)) {
            return operation_outcome(protocol::NetworkOperation::join,
                                     protocol::NetworkResultCode::pending_approval,
                                     request.network_id);
        }

        if (!store_.add_join_request(request.network_id, actor, now_ms)) {
            return operation_outcome(protocol::NetworkOperation::join,
                                     protocol::NetworkResultCode::conflict,
                                     request.network_id);
        }

        auto outcome = operation_outcome(protocol::NetworkOperation::join,
                                         protocol::NetworkResultCode::pending_approval,
                                         request.network_id);
        outcome.events.push_back(routed_event(network->owner_device_id,
                                              protocol::NetworkEventKind::join_requested,
                                              request.network_id,
                                              actor));
        return outcome;
    } catch (const std::length_error&) {
        return operation_outcome(protocol::NetworkOperation::join,
                                 protocol::NetworkResultCode::limit_reached,
                                 request.network_id);
    } catch (const std::exception&) {
        return operation_outcome(protocol::NetworkOperation::join,
                                 protocol::NetworkResultCode::internal_error,
                                 request.network_id);
    }
}

NetworkOperationOutcome NetworkService::leave_network(
    const auth::DeviceId& actor,
    const protocol::NetworkSelectionRequest& request,
    const std::int64_t now_ms) {
    require_actor_and_time(actor, now_ms);
    require_selection_request(request);
    std::lock_guard lock(mutex_);

    try {
        store_.record_device(actor, now_ms);
        const auto network = store_.find_network(request.network_id);

        if (!network) {
            return operation_outcome(protocol::NetworkOperation::leave,
                                     protocol::NetworkResultCode::not_found,
                                     request.network_id);
        }
        if (network->owner_device_id == actor) {
            return operation_outcome(protocol::NetworkOperation::leave,
                                     protocol::NetworkResultCode::permission_denied,
                                     request.network_id);
        }

        const auto members = store_.list_members(request.network_id);

        if (find_member(members, actor) == nullptr) {
            return operation_outcome(protocol::NetworkOperation::leave,
                                     protocol::NetworkResultCode::not_member,
                                     request.network_id);
        }
        if (!store_.remove_member(request.network_id, actor)) {
            return operation_outcome(protocol::NetworkOperation::leave,
                                     protocol::NetworkResultCode::conflict,
                                     request.network_id);
        }

        auto outcome = operation_outcome(protocol::NetworkOperation::leave,
                                         protocol::NetworkResultCode::success,
                                         request.network_id);
        outcome.events = events_for_members(store_.list_members(request.network_id),
                                            std::nullopt,
                                            protocol::NetworkEventKind::member_left,
                                            request.network_id,
                                            actor);
        return outcome;
    } catch (const std::exception&) {
        return operation_outcome(protocol::NetworkOperation::leave,
                                 protocol::NetworkResultCode::internal_error,
                                 request.network_id);
    }
}

NetworkOperationOutcome NetworkService::invite_member(
    const auth::DeviceId& actor,
    const protocol::NetworkMemberRequest& request,
    const std::int64_t now_ms) {
    require_actor_and_time(actor, now_ms);

    if (!valid_member_request(request)) {
        return operation_outcome(protocol::NetworkOperation::invite,
                                 protocol::NetworkResultCode::invalid_request,
                                 request.network_id);
    }

    std::lock_guard lock(mutex_);

    try {
        store_.record_device(actor, now_ms);
        const auto network = store_.find_network(request.network_id);

        if (!network) {
            return operation_outcome(protocol::NetworkOperation::invite,
                                     protocol::NetworkResultCode::not_found,
                                     request.network_id);
        }
        if (network->owner_device_id != actor) {
            return operation_outcome(protocol::NetworkOperation::invite,
                                     protocol::NetworkResultCode::permission_denied,
                                     request.network_id);
        }
        if (!store_.find_device(request.device_id)) {
            return operation_outcome(protocol::NetworkOperation::invite,
                                     protocol::NetworkResultCode::target_not_found,
                                     request.network_id);
        }

        const auto members = store_.list_members(request.network_id);

        if (find_member(members, request.device_id) != nullptr ||
            store_.find_invitation(request.network_id, request.device_id)) {
            return operation_outcome(protocol::NetworkOperation::invite,
                                     protocol::NetworkResultCode::already_exists,
                                     request.network_id);
        }
        if (store_.find_join_request(request.network_id, request.device_id)) {
            return operation_outcome(protocol::NetworkOperation::invite,
                                     protocol::NetworkResultCode::conflict,
                                     request.network_id);
        }
        if (network_limit_reached(store_, request.device_id)) {
            return operation_outcome(protocol::NetworkOperation::invite,
                                     protocol::NetworkResultCode::limit_reached,
                                     request.network_id);
        }
        if (!store_.add_invitation(request.network_id, request.device_id, now_ms)) {
            return operation_outcome(protocol::NetworkOperation::invite,
                                     protocol::NetworkResultCode::conflict,
                                     request.network_id);
        }

        auto outcome = operation_outcome(protocol::NetworkOperation::invite,
                                         protocol::NetworkResultCode::success,
                                         request.network_id);
        outcome.events.push_back(routed_event(request.device_id,
                                              protocol::NetworkEventKind::invited,
                                              request.network_id,
                                              request.device_id));
        return outcome;
    } catch (const std::exception&) {
        return operation_outcome(protocol::NetworkOperation::invite,
                                 protocol::NetworkResultCode::internal_error,
                                 request.network_id);
    }
}

NetworkOperationOutcome NetworkService::approve_member(
    const auth::DeviceId& actor,
    const protocol::NetworkMemberRequest& request,
    const std::int64_t now_ms) {
    require_actor_and_time(actor, now_ms);

    if (!valid_member_request(request)) {
        return operation_outcome(protocol::NetworkOperation::approve,
                                 protocol::NetworkResultCode::invalid_request,
                                 request.network_id);
    }

    std::lock_guard lock(mutex_);

    try {
        store_.record_device(actor, now_ms);
        const auto network = store_.find_network(request.network_id);

        if (!network) {
            return operation_outcome(protocol::NetworkOperation::approve,
                                     protocol::NetworkResultCode::not_found,
                                     request.network_id);
        }
        if (network->owner_device_id != actor) {
            return operation_outcome(protocol::NetworkOperation::approve,
                                     protocol::NetworkResultCode::permission_denied,
                                     request.network_id);
        }
        if (!store_.find_device(request.device_id)) {
            return operation_outcome(protocol::NetworkOperation::approve,
                                     protocol::NetworkResultCode::target_not_found,
                                     request.network_id);
        }

        const auto members = store_.list_members(request.network_id);

        if (find_member(members, request.device_id) != nullptr) {
            return operation_outcome(protocol::NetworkOperation::approve,
                                     protocol::NetworkResultCode::already_exists,
                                     request.network_id);
        }
        if (!store_.find_join_request(request.network_id, request.device_id)) {
            return operation_outcome(protocol::NetworkOperation::approve,
                                     protocol::NetworkResultCode::not_found,
                                     request.network_id);
        }
        if (network_limit_reached(store_, request.device_id)) {
            return operation_outcome(protocol::NetworkOperation::approve,
                                     protocol::NetworkResultCode::limit_reached,
                                     request.network_id);
        }
        if (!store_.add_member(request.network_id, request.device_id, now_ms)) {
            return operation_outcome(protocol::NetworkOperation::approve,
                                     protocol::NetworkResultCode::conflict,
                                     request.network_id);
        }

        auto outcome = operation_outcome(protocol::NetworkOperation::approve,
                                         protocol::NetworkResultCode::success,
                                         request.network_id);
        outcome.events = events_for_members(store_.list_members(request.network_id),
                                            actor,
                                            protocol::NetworkEventKind::member_joined,
                                            request.network_id,
                                            request.device_id);
        return outcome;
    } catch (const std::length_error&) {
        return operation_outcome(protocol::NetworkOperation::approve,
                                 protocol::NetworkResultCode::limit_reached,
                                 request.network_id);
    } catch (const std::exception&) {
        return operation_outcome(protocol::NetworkOperation::approve,
                                 protocol::NetworkResultCode::internal_error,
                                 request.network_id);
    }
}

NetworkOperationOutcome NetworkService::kick_member(
    const auth::DeviceId& actor,
    const protocol::NetworkMemberRequest& request,
    const std::int64_t now_ms) {
    require_actor_and_time(actor, now_ms);

    if (!valid_member_request(request)) {
        return operation_outcome(protocol::NetworkOperation::kick,
                                 protocol::NetworkResultCode::invalid_request,
                                 request.network_id);
    }

    std::lock_guard lock(mutex_);

    try {
        store_.record_device(actor, now_ms);
        const auto network = store_.find_network(request.network_id);

        if (!network) {
            return operation_outcome(protocol::NetworkOperation::kick,
                                     protocol::NetworkResultCode::not_found,
                                     request.network_id);
        }
        if (network->owner_device_id != actor) {
            return operation_outcome(protocol::NetworkOperation::kick,
                                     protocol::NetworkResultCode::permission_denied,
                                     request.network_id);
        }
        if (!store_.find_device(request.device_id)) {
            return operation_outcome(protocol::NetworkOperation::kick,
                                     protocol::NetworkResultCode::target_not_found,
                                     request.network_id);
        }
        if (request.device_id == network->owner_device_id) {
            return operation_outcome(protocol::NetworkOperation::kick,
                                     protocol::NetworkResultCode::permission_denied,
                                     request.network_id);
        }

        const auto members = store_.list_members(request.network_id);

        if (find_member(members, request.device_id) == nullptr) {
            return operation_outcome(protocol::NetworkOperation::kick,
                                     protocol::NetworkResultCode::not_member,
                                     request.network_id);
        }
        if (!store_.remove_member(request.network_id, request.device_id)) {
            return operation_outcome(protocol::NetworkOperation::kick,
                                     protocol::NetworkResultCode::conflict,
                                     request.network_id);
        }

        auto outcome = operation_outcome(protocol::NetworkOperation::kick,
                                         protocol::NetworkResultCode::success,
                                         request.network_id);
        outcome.events.push_back(routed_event(request.device_id,
                                              protocol::NetworkEventKind::member_kicked,
                                              request.network_id,
                                              request.device_id));
        auto member_events = events_for_members(store_.list_members(request.network_id),
                                                actor,
                                                protocol::NetworkEventKind::member_kicked,
                                                request.network_id,
                                                request.device_id);
        outcome.events.insert(outcome.events.end(),
                              member_events.begin(),
                              member_events.end());
        return outcome;
    } catch (const std::exception&) {
        return operation_outcome(protocol::NetworkOperation::kick,
                                 protocol::NetworkResultCode::internal_error,
                                 request.network_id);
    }
}

}
