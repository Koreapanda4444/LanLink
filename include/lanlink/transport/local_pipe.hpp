#pragma once

#include "lanlink/protocol/local_control.hpp"

#include <memory>

#include <functional>


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
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] protocol::LocalMessage local_pipe_request(
    const protocol::LocalMessage& request);

}
