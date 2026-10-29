#include "lanlink/relay/network_service.hpp"
#include "lanlink/auth/access_secret.hpp"
#include "lanlink/protocol/join_target.hpp"
#include "lanlink/auth/network_keys.hpp"
#include "lanlink/protocol/ipv4_fanout.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
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
    return {{operation, code, network_id}, {}, {}};
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

protocol::NetworkPeerState assemble_peer_state(
    const storage::NetworkSubnetRecord& subnet,
    const std::vector<storage::VirtualIpv4LeaseRecord>& leases,
    const auth::DeviceId& actor,
    const std::uint64_t revision,
    const auth::DeviceId& owner_id,
    const std::uint64_t key_epoch,
    const auto& active_keys) {
    const auto own = std::find_if(leases.begin(), leases.end(), [&](const auto& lease) {
        return lease.device_id == actor;
    });
    if (own == leases.end() || leases.size() > protocol::max_network_peer_entries + 1) {
        throw std::runtime_error("network virtual IPv4 membership state is inconsistent");
    }

    protocol::NetworkPeerState state;
    state.network_id = subnet.network_id;
    state.revision = revision;
    state.subnet_address = subnet.network_address;
    state.prefix_length = subnet.prefix_length;
    state.own_address = own->address;
    state.owner_device_id = owner_id;
    state.key_epoch = key_epoch;
    state.peers.reserve(leases.size() - 1);
    for (const auto& lease : leases) {
        if (lease.device_id != actor) {
            protocol::NetworkPeer peer{lease.device_id, lease.address};
            if (const auto key = active_keys.find(lease.device_id);
                key != active_keys.end()) {
                peer.signed_key = key->second.signed_key;
            }
            state.peers.push_back(std::move(peer));
        }
    }
    return state;
}

}

NetworkService::NetworkService(storage::RelayStore& store,
                               NetworkIdGenerator network_id_generator,
                               TimeSource time_source)
    : store_(store),
      network_id_generator_(network_id_generator ? std::move(network_id_generator)
                                                 : NetworkIdGenerator{generate_network_id}),
      time_source_(time_source ? std::move(time_source)
                               : TimeSource{[] { return Clock::now(); }}) {
}

bool NetworkService::allow_access_attempt(const auth::DeviceId& actor, const storage::NetworkId& network) {
    const auto now=time_source_();
    std::erase_if(access_attempts_,[&](const auto& item){return now-item.second.start>=std::chrono::minutes(1);});
    if(access_attempts_.size()>4092) return false;
    const std::array keys{std::pair{network,actor},std::pair{storage::NetworkId{},actor},
        std::pair{network,auth::DeviceId{}},std::pair{storage::NetworkId{},auth::DeviceId{}}};
    const std::array<unsigned,4> limits{5,10,20,40};
    for(std::size_t i=0;i<keys.size();++i) {
        auto [it,inserted]=access_attempts_.try_emplace(keys[i],AccessWindow{now,0});
        static_cast<void>(inserted);
        if(it->second.attempts>=limits[i]) return false;
    }
    for(const auto& key:keys) ++access_attempts_.at(key).attempts;
    return true;
}

bool NetworkService::device_allowed(const auth::DeviceId& actor) const {
    return !store_.device_revoked(actor);
}
protocol::AccessResult NetworkService::access_request(const auth::DeviceId& actor,
    const protocol::AccessRequest& request, const std::int64_t now_ms,
    std::vector<RoutedPeerStateChange>* changes, std::vector<RoutedNetworkEvent>* events) {
    require_actor_and_time(actor, now_ms);
    static_cast<void>(protocol::encode_access_request(request));
    std::lock_guard lock(mutex_);
    protocol::AccessResult result; result.action=request.action;
    if (!device_allowed(actor)) { result.code=protocol::NetworkResultCode::permission_denied; return result; }
    store_.record_device(actor,now_ms);
    if (request.action==protocol::AccessAction::device_revoke) {
        const auto account=store_.account_for_device(actor);
        const auto target=store_.account_for_device(request.device_id);
        if (!account || account->owner!=actor || !target || target->id!=account->id || request.device_id==actor) {
            result.code=protocol::NetworkResultCode::permission_denied; return result;
        }
        const auto networks=store_.list_networks_for_device(request.device_id);
        if (std::any_of(networks.begin(),networks.end(),[&](const auto& n){return n.owner_device_id==request.device_id;})) {
            result.code=protocol::NetworkResultCode::conflict; return result;
        }
        store_.revoke_account_device(account->id,request.device_id);
        for (const auto& n : networks) {
            static_cast<void>(store_.remove_member(n.id,request.device_id));
            advance_key_epoch(n.id); const auto revision=advance_revision(n.id);
            if (changes) {
                auto updates=changes_for_members(n.id,revision);
                changes->insert(changes->end(),updates.begin(),updates.end());
                changes->push_back({request.device_id,std::nullopt,{n.id,revision}});
            }
        }
        active_keys_.erase(request.device_id);
        result.account_id=account->id; return result;
    }
    if(request.action>=protocol::AccessAction::invite_issue) {
        result.network_id=request.network_id;
        if(!allow_access_attempt(actor,request.network_id)){result.code=protocol::NetworkResultCode::limit_reached;return result;}
        const auto network=store_.find_network(request.network_id);
        if(request.action!=protocol::AccessAction::invite_redeem) {
            if(!network || network->owner_device_id!=actor){result.code=protocol::NetworkResultCode::permission_denied;return result;}
            if(request.action==protocol::AccessAction::invite_revoke){store_.revoke_invite_links(request.network_id);return result;}
            if(request.lifetime_seconds<60 || request.lifetime_seconds>86400 || !request.max_uses || request.max_uses>100) {
                result.code=protocol::NetworkResultCode::invalid_request;return result;
            }
            const auto code=auth::random_access_code();
            store_.issue_invite_link(request.network_id,auth::access_digest(code),now_ms,
                now_ms+static_cast<std::int64_t>(request.lifetime_seconds)*1000,request.max_uses);
            result.secret=protocol::make_invite_link(request.network_id,code);
            result.text=protocol::make_invite_code(request.network_id,code);return result;
        }
        if(!network || request.secret.size()!=40) {result.code=protocol::NetworkResultCode::permission_denied;return result;}
        if(store_.find_virtual_ipv4_lease(request.network_id,actor)){result.code=protocol::NetworkResultCode::already_exists;return result;}
        if(network_limit_reached(store_,actor) || store_.list_members(request.network_id).size()>=protocol::max_network_peer_entries+1) {
            result.code=protocol::NetworkResultCode::limit_reached;return result;
        }
        if(!store_.redeem_invite_link(request.network_id,auth::access_digest(request.secret),actor,now_ms)) {
            result.code=protocol::NetworkResultCode::permission_denied;return result;
        }
        const auto joined=join_network_locked(actor,{request.network_id},now_ms,false);
        result.code=joined.result.code;
        if(changes)*changes=joined.peer_changes;
        if(events)*events=joined.events;
        return result;
    }
    if(request.action==protocol::AccessAction::password_set || request.action==protocol::AccessAction::password_join) {
        result.network_id=request.network_id;
        if(!allow_access_attempt(actor,request.network_id)) { result.code=protocol::NetworkResultCode::limit_reached; return result; }
        const auto network=store_.find_network(request.network_id);
        if(request.action==protocol::AccessAction::password_set) {
            if(!network || network->owner_device_id!=actor) { result.code=protocol::NetworkResultCode::permission_denied; return result; }
            if(request.secret.empty()) store_.set_network_password(request.network_id,std::nullopt);
            else store_.set_network_password(request.network_id,auth::protect_password(request.secret,request.require_approval));
            return result;
        }
        const auto verifier=store_.network_password(request.network_id);
        const bool matches=auth::password_matches(request.secret,verifier.value_or(auth::PasswordVerifier{}));
        if(!network || !verifier || !matches) { result.code=protocol::NetworkResultCode::permission_denied; return result; }
        const auto joined=join_network_locked(actor,{request.network_id},now_ms,true);
        result.code=joined.result.code;
        if(changes) *changes=joined.peer_changes;
        if(events) *events=joined.events;
        return result;
    }
    auto account=store_.account_for_device(actor);
    switch (request.action) {
    case protocol::AccessAction::account_create:
        if (account) { result.code=protocol::NetworkResultCode::already_exists; break; }
        if (request.text.empty()) { result.code=protocol::NetworkResultCode::invalid_request; break; }
        store_.create_account({auth::random_nonce(),actor,request.text},now_ms);
        account=store_.account_for_device(actor); break;
    case protocol::AccessAction::account_status: break;
    case protocol::AccessAction::enrollment_issue:
        if (!account || account->owner!=actor) { result.code=protocol::NetworkResultCode::permission_denied; break; }
        result.secret=auth::random_access_code();
        store_.issue_enrollment(account->id,auth::access_digest(result.secret),now_ms+300000); break;
    case protocol::AccessAction::device_enroll:
        if (request.secret.size()!=40 || !store_.enroll_device(auth::access_digest(request.secret),actor,request.text,now_ms)) {
            result.code=protocol::NetworkResultCode::permission_denied; break;
        }
        account=store_.account_for_device(actor); break;
    default: result.code=protocol::NetworkResultCode::invalid_request; break;
    }
    if (account && result.code==protocol::NetworkResultCode::success) {
        result.account_id=account->id; result.text=account->name;
        result.devices=store_.account_devices(account->id);
    } else if (!account && result.code==protocol::NetworkResultCode::success) result.code=protocol::NetworkResultCode::not_found;
    return result;
}

void NetworkService::record_authenticated_device(const auth::DeviceId& actor,
                                                 const std::int64_t now_ms) {
    require_actor_and_time(actor, now_ms);
    std::lock_guard lock(mutex_);
    if (!device_allowed(actor)) throw std::runtime_error("device enrollment revoked");
    store_.record_device(actor, now_ms);
}

std::vector<RoutedPeerStateChange> NetworkService::publish_device_key(
    const auth::DeviceId& actor,
    const auth::SessionId& session_id,
    const auth::SignedDeviceKey& signed_key,
    const std::int64_t now_ms) {
    require_actor_and_time(actor, now_ms);
    if (is_zero(session_id) || !auth::verify_signed_device_key(signed_key, actor)) {
        throw std::invalid_argument("authenticated device key is invalid");
    }

    std::lock_guard lock(mutex_);
    store_.record_device(actor, now_ms);
    if (!device_allowed(actor)) throw std::runtime_error("device enrollment revoked");
    active_keys_.insert_or_assign(actor, ActiveKey{session_id, signed_key});
    const auto networks = store_.list_networks_for_device(actor);
    std::vector<RoutedPeerStateChange> changes;
    for (const auto& network : networks) {
        if (network.owner_device_id == actor) {
            advance_key_epoch(network.id);
        }
        auto network_changes = changes_for_members(network.id, advance_revision(network.id));
        changes.insert(changes.end(),
                       std::make_move_iterator(network_changes.begin()),
                       std::make_move_iterator(network_changes.end()));
    }
    return changes;
}

std::vector<RoutedPeerStateChange> NetworkService::remove_device_key(
    const auth::DeviceId& actor, const auth::SessionId& session_id) {
    std::lock_guard lock(mutex_);
    const auto found = active_keys_.find(actor);
    if (found == active_keys_.end() || found->second.session_id != session_id) {
        return {};
    }

    active_keys_.erase(found);
    std::erase_if(sender_fanout_, [&](const auto& entry) {
        return entry.first.second == actor;
    });
    std::erase_if(fanout_replay_, [&](const auto& entry) { return entry.first.second == actor; });
    const auto networks = store_.list_networks_for_device(actor);
    std::vector<RoutedPeerStateChange> changes;
    for (const auto& network : networks) {
        if (network.owner_device_id == actor) {
            advance_key_epoch(network.id);
        }
        auto network_changes = changes_for_members(network.id, advance_revision(network.id));
        changes.insert(changes.end(),
                       std::make_move_iterator(network_changes.begin()),
                       std::make_move_iterator(network_changes.end()));
    }
    return changes;
}

std::vector<RoutedEncryptedPacket> NetworkService::route_encrypted_packet(
    const auth::DeviceId& actor,
    const protocol::EncryptedNetworkPacket& packet) {
    if (is_zero(actor) || !device_allowed(actor)) {
        return {};
    }
    static_cast<void>(protocol::encode_encrypted_network_packet(packet));

    std::lock_guard lock(mutex_);
    if (!store_.find_network(packet.network_id) ||
        packet.key_epoch != key_epoch_for(packet.network_id) ||
        !active_keys_.contains(actor)) {
        return {};
    }
    const auto sender = store_.find_virtual_ipv4_lease(packet.network_id, actor);
    if (!sender || sender->address == packet.destination_ipv4) {
        return {};
    }
    const auto subnet = store_.find_subnet(packet.network_id);
    if (!subnet) {
        return {};
    }
    const bool fanout = protocol::is_ipv4_fanout_destination(
        packet.destination_ipv4, subnet->network_address, subnet->prefix_length);
    const auto leases = store_.list_virtual_ipv4_leases(packet.network_id);
    if (fanout) {
        if (packet.ciphertext.size() > 1400) {
            return {};
        }
        auto& replay = fanout_replay_[{packet.network_id, actor}];
        const auto& session_key = active_keys_.at(actor).signed_key.encryption_public_key;
        if (replay.epoch != packet.key_epoch || replay.session_key != session_key)
            replay = FanoutReplay{packet.key_epoch, session_key, {}, 0, 0};
        if (replay.highest && (!std::equal(replay.nonce_prefix.begin(), replay.nonce_prefix.end(), packet.nonce.begin()) ||
            (packet.sequence <= replay.highest && (replay.highest - packet.sequence >= 64 ||
             (replay.seen & (std::uint64_t{1} << (replay.highest - packet.sequence))))))) return {};
        std::vector<RoutedEncryptedPacket> deliveries;
        for (const auto& lease : leases) {
            if (lease.device_id != actor && active_keys_.contains(lease.device_id)) {
                deliveries.push_back({lease.device_id, {actor, packet}});
            }
        }
        if (deliveries.empty()) {
            return {};
        }
        const auto now = time_source_();
        const auto replenish = [now](FanoutBucket& bucket,
                                     const double capacity,
                                     const double per_second) {
            if (!bucket.initialized) {
                bucket.tokens = capacity;
                bucket.initialized = true;
            } else if (now > bucket.updated) {
                const auto seconds = std::chrono::duration<double>(
                    now - bucket.updated).count();
                bucket.tokens = std::min(capacity,
                                         bucket.tokens + seconds * per_second);
            }
            bucket.updated = now;
        };
        auto& sender_bucket = sender_fanout_[{packet.network_id, actor}];
        auto& network_bucket = network_fanout_[packet.network_id];
        replenish(sender_bucket, 16, 8);
        replenish(network_bucket, 256, 128);
        if (sender_bucket.tokens < 1 ||
            network_bucket.tokens < static_cast<double>(deliveries.size())) {
            return {};
        }
        sender_bucket.tokens -= 1;
        network_bucket.tokens -= static_cast<double>(deliveries.size());
        if (!replay.highest) std::copy_n(packet.nonce.begin(), replay.nonce_prefix.size(), replay.nonce_prefix.begin());
        if (packet.sequence > replay.highest) {
            const auto distance = packet.sequence - replay.highest;
            replay.seen = distance >= 64 ? 1 : (replay.seen << distance) | 1;
            replay.highest = packet.sequence;
        } else replay.seen |= std::uint64_t{1} << (replay.highest - packet.sequence);
        return deliveries;
    }
    const auto destination = std::find_if(leases.begin(), leases.end(),
        [&](const auto& lease) {
            return lease.address == packet.destination_ipv4;
        });
    if (destination == leases.end() ||
        !active_keys_.contains(destination->device_id)) {
        return {};
    }
    return {RoutedEncryptedPacket{destination->device_id, {actor, packet}}};
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

            auto outcome = operation_outcome(protocol::NetworkOperation::create,
                                             protocol::NetworkResultCode::success,
                                             network_id);
            advance_key_epoch(network_id);
            outcome.peer_changes = changes_for_members(network_id,
                                                       advance_revision(network_id));
            return outcome;
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
    return join_network_locked(actor,request,now_ms,false);
}
NetworkOperationOutcome NetworkService::join_network_locked(const auth::DeviceId& actor,
    const protocol::NetworkSelectionRequest& request, const std::int64_t now_ms, bool credential_verified) {
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

        const auto password=store_.network_password(request.network_id);
        const bool invited=store_.find_invitation(request.network_id,actor).has_value();
        if(password && !credential_verified && !invited)
            return operation_outcome(protocol::NetworkOperation::join,protocol::NetworkResultCode::invite_required,request.network_id);
        if (invited || (credential_verified && password && !password->require_approval)) {
            if (!store_.add_member(request.network_id, actor, now_ms)) {
                return operation_outcome(protocol::NetworkOperation::join,
                                         protocol::NetworkResultCode::conflict,
                                         request.network_id);
            }

            auto outcome = operation_outcome(protocol::NetworkOperation::join,
                                             protocol::NetworkResultCode::success,
                                             request.network_id);
            advance_key_epoch(request.network_id);
            outcome.events = events_for_members(store_.list_members(request.network_id),
                                                actor,
                                                protocol::NetworkEventKind::member_joined,
                                                request.network_id,
                                                actor);
            outcome.peer_changes = changes_for_members(
                request.network_id, advance_revision(request.network_id));
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
        advance_key_epoch(request.network_id);
        outcome.events = events_for_members(store_.list_members(request.network_id),
                                            std::nullopt,
                                            protocol::NetworkEventKind::member_left,
                                            request.network_id,
                                            actor);
        const auto revision = advance_revision(request.network_id);
        outcome.peer_changes = changes_for_members(request.network_id, revision);
        outcome.peer_changes.push_back({actor, std::nullopt, {request.network_id, revision}});
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
        advance_key_epoch(request.network_id);
        outcome.events = events_for_members(store_.list_members(request.network_id),
                                            actor,
                                            protocol::NetworkEventKind::member_joined,
                                            request.network_id,
                                            request.device_id);
        outcome.peer_changes = changes_for_members(
            request.network_id, advance_revision(request.network_id));
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
        advance_key_epoch(request.network_id);
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
        const auto revision = advance_revision(request.network_id);
        outcome.peer_changes = changes_for_members(request.network_id, revision);
        outcome.peer_changes.push_back(
            {request.device_id, std::nullopt, {request.network_id, revision}});
        return outcome;
    } catch (const std::exception&) {
        return operation_outcome(protocol::NetworkOperation::kick,
                                 protocol::NetworkResultCode::internal_error,
                                 request.network_id);
    }
}

std::uint64_t NetworkService::revision_for(const storage::NetworkId& network_id) const {
    const auto found = revisions_.find(network_id);
    return found == revisions_.end() ? 0 : found->second;
}

std::uint64_t NetworkService::advance_revision(const storage::NetworkId& network_id) {
    auto& revision = revisions_[network_id];
    if (revision == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("network peer state revision exhausted");
    }
    return ++revision;
}

std::uint64_t NetworkService::key_epoch_for(const storage::NetworkId& network_id) const {
    const auto found = key_epochs_.find(network_id);
    return found == key_epochs_.end() ? 1 : found->second;
}

void NetworkService::advance_key_epoch(const storage::NetworkId& network_id) {
    auto& epoch = key_epochs_[network_id];
    if (epoch == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("network key epoch exhausted");
    }
    ++epoch;
}

protocol::NetworkPeerState NetworkService::make_peer_state(
    const storage::NetworkId& network_id,
    const auth::DeviceId& actor,
    const std::uint64_t revision) const {
    const auto subnet = store_.find_subnet(network_id);
    const auto network = store_.find_network(network_id);
    if (!subnet || !network) {
        throw std::runtime_error("network virtual IPv4 subnet is missing");
    }
    return assemble_peer_state(*subnet, store_.list_virtual_ipv4_leases(network_id),
                               actor, revision, network->owner_device_id,
                               key_epoch_for(network_id), active_keys_);
}

std::vector<RoutedPeerStateChange> NetworkService::changes_for_members(
    const storage::NetworkId& network_id, const std::uint64_t revision) const {
    const auto subnet = store_.find_subnet(network_id);
    const auto network = store_.find_network(network_id);
    if (!subnet || !network) {
        throw std::runtime_error("network virtual IPv4 subnet is missing");
    }
    const auto leases = store_.list_virtual_ipv4_leases(network_id);
    std::vector<RoutedPeerStateChange> result;
    result.reserve(leases.size());
    for (const auto& lease : leases) {
        result.push_back({lease.device_id,
                          assemble_peer_state(*subnet, leases, lease.device_id, revision,
                                              network->owner_device_id,
                                              key_epoch_for(network_id), active_keys_),
                          {}});
    }
    return result;
}

NetworkKeyPublishOutcome NetworkService::publish_network_keys(
    const auth::DeviceId& actor,
    const protocol::NetworkKeyPublishRequest& request) {
    if (is_zero(actor) || request.envelopes.empty() ||
        request.envelopes.size() > protocol::max_network_peer_entries) {
        return {protocol::NetworkResultCode::invalid_request, {}, {}};
    }

    const auto network_id = request.envelopes.front().network_id;
    std::lock_guard lock(mutex_);
    const auto network = store_.find_network(network_id);
    if (!network) {
        return {protocol::NetworkResultCode::not_found, network_id, {}};
    }
    const auto owner_key = active_keys_.find(actor);
    if (network->owner_device_id != actor || owner_key == active_keys_.end()) {
        return {protocol::NetworkResultCode::permission_denied, network_id, {}};
    }

    std::set<auth::DeviceId> recipients;
    for (const auto& envelope : request.envelopes) {
        if (envelope.network_id != network_id ||
            envelope.owner_device_id != actor ||
            envelope.recipient_device_id == actor ||
            !recipients.insert(envelope.recipient_device_id).second ||
            owner_key->second.signed_key.encryption_public_key !=
                envelope.owner_encryption_public_key ||
            !auth::verify_network_key_envelope(
                envelope, owner_key->second.signed_key.identity_public_key)) {
            return {protocol::NetworkResultCode::invalid_request, network_id, {}};
        }
        if (envelope.epoch != key_epoch_for(network_id)) {
            return {protocol::NetworkResultCode::conflict, network_id, {}};
        }
        const auto recipient = active_keys_.find(envelope.recipient_device_id);
        if (!store_.find_virtual_ipv4_lease(network_id, envelope.recipient_device_id) ||
            recipient == active_keys_.end() ||
            recipient->second.signed_key.encryption_public_key !=
                envelope.recipient_encryption_public_key) {
            return {protocol::NetworkResultCode::target_not_found, network_id, {}};
        }
    }
    return {protocol::NetworkResultCode::success, network_id, request.envelopes};
}

NetworkPeerStateOutcome NetworkService::peer_state(
    const auth::DeviceId& actor,
    const protocol::NetworkSelectionRequest& request,
    const std::int64_t now_ms) {
    require_actor_and_time(actor, now_ms);
    require_selection_request(request);
    std::lock_guard lock(mutex_);

    try {
        store_.record_device(actor, now_ms);
        if (!store_.find_network(request.network_id)) {
            return {protocol::NetworkResultCode::not_found, std::nullopt};
        }
        if (!store_.find_virtual_ipv4_lease(request.network_id, actor)) {
            return {protocol::NetworkResultCode::not_member, std::nullopt};
        }
        return {protocol::NetworkResultCode::success,
                make_peer_state(request.network_id, actor,
                                revision_for(request.network_id))};
    } catch (const std::exception&) {
        return {protocol::NetworkResultCode::internal_error, std::nullopt};
    }
}

std::vector<RoutedPeerStateChange> NetworkService::peer_states_for_device(
    const auth::DeviceId& actor) {
    if (is_zero(actor)) {
        throw std::invalid_argument("peer state requires an authenticated device");
    }
    std::lock_guard lock(mutex_);
    const auto networks = store_.list_networks_for_device(actor);
    std::vector<RoutedPeerStateChange> result;
    result.reserve(networks.size());
    for (const auto& network : networks) {
        result.push_back({actor,
                          make_peer_state(network.id, actor, revision_for(network.id)),
                          {}});
    }
    return result;
}

}

namespace lanlink::relay {
bool NetworkService::authorize_peer_exchange(const auth::DeviceId& actor, const p2p::CandidateExchange& x) {
    std::lock_guard lock(mutex_);
    if (!device_allowed(actor) || !device_allowed(x.recipient) ||
        actor != x.sender || actor == x.recipient ||
        !store_.find_virtual_ipv4_lease(x.network_id, actor) ||
        !store_.find_virtual_ipv4_lease(x.network_id, x.recipient) || key_epoch_for(x.network_id) != x.key_epoch) return false;
    const auto a=active_keys_.find(actor), b=active_keys_.find(x.recipient);
    return a!=active_keys_.end() && b!=active_keys_.end() &&
        a->second.signed_key.encryption_public_key==x.sender_session &&
        b->second.signed_key.encryption_public_key==x.recipient_session;
}
}
