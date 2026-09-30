#pragma once

#include "lanlink/protocol/local_control.hpp"

#include <windows.h>

#include <functional>
#include <thread>

namespace lanlink::transport {

class LocalPipeServer {
public:
    using Handler = std::function<protocol::LocalMessage(const protocol::LocalMessage&)>;

    explicit LocalPipeServer(Handler handler);
    ~LocalPipeServer();

    LocalPipeServer(const LocalPipeServer&) = delete;
    LocalPipeServer& operator=(const LocalPipeServer&) = delete;

    void stop() noexcept;

private:
    void serve() noexcept;

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE stopping_ = nullptr;
    Handler handler_;
    std::thread thread_;
};

[[nodiscard]] protocol::LocalMessage local_pipe_request(
    const protocol::LocalMessage& request);

}
