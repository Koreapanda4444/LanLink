#include "lanlink/core/component.hpp"
#include "lanlink/protocol/local_control.hpp"
#include "lanlink/protocol/network_messages.hpp"
#include "lanlink/transport/local_pipe.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string utf8(const std::wstring_view text) {
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length == 0) throw std::invalid_argument("invalid Unicode text");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            text.data(), static_cast<int>(text.size()), result.data(), length,
            nullptr, nullptr)) {
        throw std::invalid_argument("invalid Unicode text");
    }
    return result;
}

unsigned hex_digit(const wchar_t digit) {
    if (digit >= L'0' && digit <= L'9') return digit - L'0';
    if (digit >= L'a' && digit <= L'f') return digit - L'a' + 10;
    if (digit >= L'A' && digit <= L'F') return digit - L'A' + 10;
    throw std::invalid_argument("invalid hex identifier");
}

template <std::size_t N>
std::array<std::byte, N> parse_id(const std::wstring_view text) {
    if (text.size() != N * 2) throw std::invalid_argument("invalid identifier length");
    std::array<std::byte, N> id{};
    for (std::size_t i = 0; i < N; ++i) {
        id[i] = static_cast<std::byte>((hex_digit(text[i * 2]) << 4) |
                                        hex_digit(text[i * 2 + 1]));
    }
    return id;
}

std::string hex_id(const std::span<const std::byte> id) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(id.size() * 2);
    for (const auto byte : id) {
        const auto value = std::to_integer<unsigned>(byte);
        text += digits[value >> 4];
        text += digits[value & 15];
    }
    return text;
}

lanlink::protocol::LocalCommand command_from(const std::wstring_view action) {
    using lanlink::protocol::LocalCommand;
    if (action == L"status") return LocalCommand::status;
    if (action == L"list") return LocalCommand::list;
    if (action == L"create") return LocalCommand::create;
    if (action == L"join") return LocalCommand::join;
    if (action == L"leave") return LocalCommand::leave;
    if (action == L"invite") return LocalCommand::invite;
    if (action == L"approve") return LocalCommand::approve;
    if (action == L"kick") return LocalCommand::kick;
    if (action == L"stop") return LocalCommand::stop;
    throw std::invalid_argument("unknown local control command");
}

}

int wmain(const int argc, wchar_t* argv[]) {
    try {
        namespace protocol = lanlink::protocol;
        const auto action = argc > 1 ? std::wstring_view{argv[1]} : L"status";
        const auto command = command_from(action);
        protocol::LocalMessage request{command, false, false, {}};
        switch (command) {
        case protocol::LocalCommand::status:
        case protocol::LocalCommand::stop:
            if (argc != 1 && argc != 2) {
                throw std::invalid_argument("status and stop take no arguments");
            }
            break;
        case protocol::LocalCommand::list:
            if (argc != 2) throw std::invalid_argument("list takes no arguments");
            request.payload = protocol::encode_network_list_request();
            break;
        case protocol::LocalCommand::create:
            if (argc != 3) throw std::invalid_argument("create requires a name");
            request.payload = protocol::encode_network_create_request({utf8(argv[2])});
            break;
        case protocol::LocalCommand::join:
        case protocol::LocalCommand::leave:
            if (argc != 3) throw std::invalid_argument("join/leave requires a network ID");
            request.payload = protocol::encode_network_selection_request(
                {parse_id<protocol::network_id_size>(argv[2])});
            break;
        case protocol::LocalCommand::invite:
        case protocol::LocalCommand::approve:
        case protocol::LocalCommand::kick:
            if (argc != 4) {
                throw std::invalid_argument("member command requires network and device IDs");
            }
            request.payload = protocol::encode_network_member_request(
                {parse_id<protocol::network_id_size>(argv[2]),
                 parse_id<protocol::network_device_id_size>(argv[3])});
            break;
        }

        const auto response = lanlink::transport::local_pipe_request(request);
        std::cout << "LanLink " << lanlink::core::component_name(
            lanlink::core::Component::ui) << " " << lanlink::core::project_version() << '\n';
        if (command == protocol::LocalCommand::status) {
            if (response.payload.size() != 8 || response.payload[3] != std::byte{0}) {
                throw std::runtime_error("invalid service status");
            }
            std::uint32_t networks = 0;
            for (std::size_t i = 0; i < 4; ++i) {
                networks |= std::to_integer<std::uint32_t>(response.payload[4 + i]) << (8 * i);
            }
            std::cout << "connected: " << (response.payload[0] != std::byte{0})
                      << "\nauthenticated: " << (response.payload[1] != std::byte{0})
                      << "\ntcp fallback: " << (response.payload[2] != std::byte{0})
                      << "\nactive networks: " << networks << '\n';
        } else if (command == protocol::LocalCommand::list) {
            const auto networks = protocol::decode_network_list_result(response.payload);
            for (const auto& network : networks.networks) {
                std::cout << hex_id(network.network_id) << "  " << network.name
                          << "  members=" << network.member_count << '\n';
            }
        } else if (command == protocol::LocalCommand::stop) {
            if (!response.payload.empty()) throw std::runtime_error("invalid stop response");
            std::cout << "service stopping\n";
        } else {
            const auto result = protocol::decode_network_operation_result(response.payload);
            std::cout << protocol::network_result_code_name(result.code)
                      << "  " << hex_id(result.network_id) << '\n';
            if (result.code != protocol::NetworkResultCode::success &&
                result.code != protocol::NetworkResultCode::pending_approval) {
                return EXIT_FAILURE;
            }
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "lanlink-ui: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
