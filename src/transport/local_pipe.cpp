#include "lanlink/transport/local_pipe.hpp"

#include <sddl.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lanlink::transport {
namespace {

constexpr wchar_t pipe_name[] = LR"(\\.\pipe\LanLink.Service)";
constexpr DWORD io_timeout_ms = 15000;

class Handle {
public:
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept : value_(handle) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE && value_) CloseHandle(value_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(value_, INVALID_HANDLE_VALUE);
    }
private:
    HANDLE value_;
};

[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string{operation} + " failed (Windows error " +
                             std::to_string(GetLastError()) + ")");
}

bool finish_io(HANDLE file, OVERLAPPED& operation, HANDLE stopping,
               const DWORD timeout, DWORD& transferred) {
    HANDLE events[]{operation.hEvent, stopping};
    const auto count = stopping ? 2UL : 1UL;
    const auto result = WaitForMultipleObjects(count, events, FALSE, timeout);
    if (result != WAIT_OBJECT_0) {
        CancelIoEx(file, &operation);
        DWORD ignored = 0;
        GetOverlappedResult(file, &operation, &ignored, TRUE);
        return false;
    }
    return GetOverlappedResult(file, &operation, &transferred, FALSE) != FALSE;
}

bool connect_pipe(HANDLE pipe, HANDLE stopping) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (event.get() == INVALID_HANDLE_VALUE || !event.get()) {
        fail("CreateEventW");
    }
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &operation)) {
        return true;
    }
    const auto error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) {
        return true;
    }
    if (error != ERROR_IO_PENDING) {
        return false;
    }
    DWORD transferred = 0;
    return finish_io(pipe, operation, stopping, INFINITE, transferred);
}

DWORD transfer(HANDLE pipe, void* buffer, DWORD size, bool writing,
               HANDLE stopping, DWORD timeout) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (event.get() == INVALID_HANDLE_VALUE || !event.get()) {
        fail("CreateEventW");
    }
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD transferred = 0;
    const auto success = writing
        ? WriteFile(pipe, buffer, size, &transferred, &operation)
        : ReadFile(pipe, buffer, size, &transferred, &operation);
    if (!success) {
        if (GetLastError() != ERROR_IO_PENDING ||
            !finish_io(pipe, operation, stopping, timeout, transferred)) {
            throw std::runtime_error("local pipe transfer failed or timed out");
        }
    }
    return transferred;
}

std::vector<std::byte> receive(HANDLE pipe, HANDLE stopping, DWORD timeout) {
    std::vector<std::byte> bytes(protocol::local_control_header_size +
                                 protocol::max_local_control_payload);
    const auto count = transfer(pipe, bytes.data(), static_cast<DWORD>(bytes.size()),
                                false, stopping, timeout);
    bytes.resize(count);
    return bytes;
}

void send(HANDLE pipe, const protocol::LocalMessage& message,
          HANDLE stopping, DWORD timeout) {
    auto bytes = protocol::encode_local_message(message);
    if (transfer(pipe, bytes.data(), static_cast<DWORD>(bytes.size()),
                 true, stopping, timeout) != bytes.size()) {
        throw std::runtime_error("incomplete local pipe write");
    }
}

}

LocalPipeServer::LocalPipeServer(Handler handler) : handler_(std::move(handler)) {
    if (!handler_) {
        throw std::invalid_argument("missing local pipe handler");
    }
    Handle stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (stop_event.get() == INVALID_HANDLE_VALUE || !stop_event.get()) {
        fail("CreateEventW");
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x120183;;;IU)",
            SDDL_REVISION_1, &descriptor, nullptr)) {
        fail("ConvertStringSecurityDescriptorToSecurityDescriptorW");
    }
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    pipe_ = CreateNamedPipeW(pipe_name,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 65536, 65536, 0, &security);
    const auto error = GetLastError();
    LocalFree(descriptor);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        SetLastError(error);
        fail("CreateNamedPipeW");
    }
    stopping_ = stop_event.release();
    try {
        thread_ = std::thread([this] { serve(); });
    } catch (...) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        CloseHandle(stopping_);
        stopping_ = nullptr;
        throw;
    }
}

LocalPipeServer::~LocalPipeServer() {
    stop();
}

void LocalPipeServer::stop() noexcept {
    if (stopping_) {
        SetEvent(stopping_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (stopping_) {
        CloseHandle(stopping_);
        stopping_ = nullptr;
    }
}

void LocalPipeServer::serve() noexcept {
    while (WaitForSingleObject(stopping_, 0) != WAIT_OBJECT_0) {
        try {
            if (!connect_pipe(pipe_, stopping_)) {
                continue;
            }
            const auto request = protocol::decode_local_message(
                receive(pipe_, stopping_, io_timeout_ms));
            if (request.response || request.error) {
                throw std::invalid_argument("expected a local control request");
            }
            protocol::LocalMessage response;
            try {
                response = handler_(request);
                if (!response.response || response.command != request.command) {
                    throw std::runtime_error("invalid local control response");
                }
            } catch (const std::exception& error) {
                const auto message = std::string{error.what()};
                response = {request.command, true, true,
                    {reinterpret_cast<const std::byte*>(message.data()),
                     reinterpret_cast<const std::byte*>(message.data() + message.size())}};
            }
            send(pipe_, response, stopping_, io_timeout_ms);
        } catch (...) {
        }
        DisconnectNamedPipe(pipe_);
    }
}

protocol::LocalMessage local_pipe_request(const protocol::LocalMessage& request) {
    if (request.response || request.error) {
        throw std::invalid_argument("expected a local control request");
    }
    if (!WaitNamedPipeW(pipe_name, 3000)) {
        fail("WaitNamedPipeW");
    }
    Handle pipe(CreateFileW(pipe_name, FILE_READ_DATA | FILE_WRITE_DATA |
                           FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
                           READ_CONTROL | SYNCHRONIZE, 0, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (pipe.get() == INVALID_HANDLE_VALUE) {
        fail("CreateFileW");
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) {
        fail("SetNamedPipeHandleState");
    }
    send(pipe.get(), request, nullptr, io_timeout_ms);
    const auto response = protocol::decode_local_message(
        receive(pipe.get(), nullptr, io_timeout_ms));
    if (!response.response || response.command != request.command) {
        throw std::runtime_error("unexpected local control response");
    }
    if (response.error) {
        throw std::runtime_error(std::string{
            reinterpret_cast<const char*>(response.payload.data()),
            response.payload.size()});
    }
    return response;
}

}
