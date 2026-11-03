#pragma once
#include "lanlink/transport/relay_probe.hpp"
#include <optional>
#include <string>
#include <vector>

namespace lanlink::transport {
// Owned by the connection worker. Only authenticated, fresh observations are eligible.
class RelayPool {
public:
    using Clock=std::chrono::steady_clock;
    explicit RelayPool(std::vector<auth::RelayEndpoint> endpoints);
    void update(std::vector<auth::RelayEndpoint> endpoints);
    [[nodiscard]] std::vector<auth::RelayEndpoint> due(Clock::time_point now) const;
    void observe(const auth::RelayEndpoint&,const RelayProbe&,Clock::time_point now);
    void failed(const std::string& id,Clock::time_point now);
    void activate(const std::string& id,Clock::time_point now);
    [[nodiscard]] std::optional<auth::RelayEndpoint> choose(Clock::time_point now) const;
private:
    struct Node {
        auth::RelayEndpoint endpoint;
        std::optional<RelayProbe> sample;
        Clock::time_point observed{},retry{};
        unsigned failures=0;
    };
    std::vector<Node> nodes_;
    std::string authority_,active_;
    Clock::time_point activated_{};
};
}
