#include "lanlink/relay/network_control_channel.hpp"

#include "lanlink/protocol/network_messages.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace lanlink::relay {
namespace {

template <std::size_t Size>
bool is_zero(const std::array<std::byte, Size>& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
        return byte == std::byte{0};
    });
}

std::int64_t system_time_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

protocol::Frame error_response(const std::uint32_t request_id) {
    return {protocol::MessageType::error, request_id, {}};
}

bool is_network_request(const protocol::MessageType type) noexcept {
    switch (type) {
        case protocol::MessageType::network_create_request:
        case protocol::MessageType::network_list_request:
        case protocol::MessageType::network_join_request:
        case protocol::MessageType::network_leave_request:
        case protocol::MessageType::network_invite_request:
        case protocol::MessageType::network_approve_request:
        case protocol::MessageType::network_kick_request:
            return true;
        default:
            return false;
    }
}

std::vector<RoutedControlFrame> encode_events(
    const std::vector<RoutedNetworkEvent>& events) {
    std::vector<RoutedControlFrame> result;
    result.reserve(events.size());

    for (const auto& routed : events) {
        result.push_back({
            routed.recipient_device_id,
            {protocol::MessageType::network_event,
             0,
             protocol::encode_network_event(routed.event)},
        });
    }

    return result;
}

ControlDispatch operation_response(const std::uint32_t request_id,
                                   NetworkOperationOutcome outcome) {
    protocol::Frame response{
        protocol::MessageType::network_operation_result,
        request_id,
        protocol::encode_network_operation_result(outcome.result),
    };
    return {std::move(response), encode_events(outcome.events)};
}

ControlDispatch list_response(const std::uint32_t request_id, NetworkListOutcome outcome) {
    if (outcome.code == protocol::NetworkResultCode::success) {
        return {{protocol::MessageType::network_list_result,
                 request_id,
                 protocol::encode_network_list_result(outcome.result)},
                {}};
    }

    const protocol::NetworkOperationResult result{
        protocol::NetworkOperation::list,
        outcome.code,
        {},
    };
    return {{protocol::MessageType::network_operation_result,
             request_id,
             protocol::encode_network_operation_result(result)},
            {}};
}

}

NetworkControlChannel::NetworkControlChannel(NetworkService& service, TimeSource time_source)
    : service_(service),
      time_source_(time_source ? std::move(time_source) : TimeSource{system_time_ms}) {
}

void NetworkControlChannel::note_authenticated(const auth::DeviceId& actor) {
    service_.record_authenticated_device(actor, now_ms());
}

ControlDispatch NetworkControlChannel::handle_authenticated(
    const auth::DeviceId& actor,
    const protocol::Frame& request) {
    if (is_zero(actor)) {
        throw std::invalid_argument("control request requires an authenticated device");
    }
    if (request.request_id == 0) {
        throw std::invalid_argument("control request id must be non-zero");
    }
    if (!is_network_request(request.type)) {
        throw std::runtime_error("unexpected frame on authenticated control channel");
    }

    try {
        switch (request.type) {
            case protocol::MessageType::network_create_request:
                return operation_response(
                    request.request_id,
                    service_.create_network(
                        actor,
                        protocol::decode_network_create_request(request.payload),
                        now_ms()));
            case protocol::MessageType::network_list_request:
                return list_response(
                    request.request_id,
                    service_.list_networks(
                        actor,
                        protocol::decode_network_list_request(request.payload),
                        now_ms()));
            case protocol::MessageType::network_join_request:
                return operation_response(
                    request.request_id,
                    service_.join_network(
                        actor,
                        protocol::decode_network_selection_request(request.payload),
                        now_ms()));
            case protocol::MessageType::network_leave_request:
                return operation_response(
                    request.request_id,
                    service_.leave_network(
                        actor,
                        protocol::decode_network_selection_request(request.payload),
                        now_ms()));
            case protocol::MessageType::network_invite_request:
                return operation_response(
                    request.request_id,
                    service_.invite_member(
                        actor,
                        protocol::decode_network_member_request(request.payload),
                        now_ms()));
            case protocol::MessageType::network_approve_request:
                return operation_response(
                    request.request_id,
                    service_.approve_member(
                        actor,
                        protocol::decode_network_member_request(request.payload),
                        now_ms()));
            case protocol::MessageType::network_kick_request:
                return operation_response(
                    request.request_id,
                    service_.kick_member(
                        actor,
                        protocol::decode_network_member_request(request.payload),
                        now_ms()));
            default:
                throw std::runtime_error("unexpected frame on authenticated control channel");
        }
    } catch (const std::invalid_argument&) {
        return {error_response(request.request_id), {}};
    } catch (const std::length_error&) {
        return {error_response(request.request_id), {}};
    } catch (const std::runtime_error&) {
        return {error_response(request.request_id), {}};
    }
}

std::int64_t NetworkControlChannel::now_ms() const {
    const auto result = time_source_();

    if (result < 0) {
        throw std::runtime_error("control channel time source returned a negative value");
    }

    return result;
}

}
