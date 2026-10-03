#include "lanlink/transport/local_pipe.hpp"

#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

class ClientPipe {
public:
    ClientPipe() {
        constexpr wchar_t name[] = LR"(\\.\pipe\LanLink.Service)";
        if (!WaitNamedPipeW(name, 3000)) throw std::runtime_error("test pipe was unavailable");
        pipe_ = CreateFileW(name, FILE_READ_DATA | FILE_WRITE_DATA |
                           FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
                           READ_CONTROL | SYNCHRONIZE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe_ == INVALID_HANDLE_VALUE) throw std::runtime_error("test pipe open failed");
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr)) {
            CloseHandle(pipe_);
            throw std::runtime_error("test pipe mode failed");
        }
    }

    ~ClientPipe() { CloseHandle(pipe_); }
    ClientPipe(const ClientPipe&) = delete;
    ClientPipe& operator=(const ClientPipe&) = delete;

    void send() {
        const auto bytes = lanlink::protocol::encode_local_message(
            {lanlink::protocol::LocalCommand::status, false, false, {}});
        DWORD transferred = 0;
        if (!WriteFile(pipe_, bytes.data(), static_cast<DWORD>(bytes.size()), &transferred, nullptr) ||
            transferred != bytes.size()) {
            throw std::runtime_error("test pipe write failed");
        }
    }

    void read() {
        std::vector<std::byte> bytes(lanlink::protocol::local_control_header_size + 8);
        DWORD transferred = 0;
        if (!ReadFile(pipe_, bytes.data(), static_cast<DWORD>(bytes.size()), &transferred, nullptr)) {
            throw std::runtime_error("a delayed client lost its buffered response");
        }
        bytes.resize(transferred);
        const auto response = lanlink::protocol::decode_local_message(bytes);
        if (!response.response || response.error || response.payload.size() != 8 ||
            response.payload[0] != std::byte{1}) {
            throw std::runtime_error("delayed local pipe response mismatch");
        }
    }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

}

int main() {
    try {
        using namespace lanlink;
        transport::LocalPipeServer server([](const protocol::LocalMessage& request) {
            if (request.command != protocol::LocalCommand::status) {
                throw std::invalid_argument("unsupported command");
            }
            return protocol::LocalMessage{request.command, true, false,
                {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
                 std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}}};
        });
        {
            ClientPipe client;
            client.send();
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            client.read();
        }
        for (int index = 0; index < 32; ++index) {
            const auto response = transport::local_pipe_request(
                {protocol::LocalCommand::status, false, false, {}});
            if (response.payload.size() != 8 || response.payload[0] != std::byte{1}) {
                throw std::runtime_error("local pipe response mismatch");
            }
        }
        bool rejected = false;
        try {
            static_cast<void>(transport::local_pipe_request(
                {protocol::LocalCommand::list, false, false, {}}));
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        if (!rejected) throw std::runtime_error("local pipe error was not returned");
        ClientPipe unread_client;
        unread_client.send();
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        const auto stopping = std::chrono::steady_clock::now();
        server.stop();
        if (std::chrono::steady_clock::now() - stopping > std::chrono::seconds{3}) {
            throw std::runtime_error("server shutdown waited for an unread response");
        }
        std::cout << "local pipe requests validated\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
