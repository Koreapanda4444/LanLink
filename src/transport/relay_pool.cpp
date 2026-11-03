#include "lanlink/transport/relay_pool.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace lanlink::transport {
RelayPool::RelayPool(std::vector<auth::RelayEndpoint> endpoints){update(std::move(endpoints));}
void RelayPool::update(std::vector<auth::RelayEndpoint> endpoints) {
    if(endpoints.empty() || endpoints.size()>32)throw std::invalid_argument("invalid relay pool size");
    const auto authority=authority_.empty()?endpoints.front().authority:authority_;
    std::set<std::string> ids;std::vector<Node> next;
    for(auto& endpoint:endpoints) {
        if(authority.empty() || endpoint.authority!=authority || endpoint.health_key==auth::PublicKey{} ||
           endpoint.protocol_version!=protocol::protocol_version || !endpoint.capacity || !endpoint.port ||
           !ids.insert(endpoint.id).second)throw std::invalid_argument("untrusted or incompatible relay pool");
        const auto found=std::find_if(nodes_.begin(),nodes_.end(),[&](const auto& n){return n.endpoint==endpoint;});
        if(found!=nodes_.end())next.push_back(*found);else next.push_back({std::move(endpoint),{},{},{},0});
    }
    nodes_=std::move(next);authority_=authority;
}
std::vector<auth::RelayEndpoint> RelayPool::due(Clock::time_point now) const {
    std::vector<auth::RelayEndpoint> result;
    for(const auto& n:nodes_)if(now>=n.retry && (!n.sample || now-n.observed>=std::chrono::seconds(2)))result.push_back(n.endpoint);
    return result;
}
void RelayPool::observe(const auth::RelayEndpoint& endpoint,const RelayProbe& sample,Clock::time_point now) {
    const auto n=std::find_if(nodes_.begin(),nodes_.end(),[&](const auto& x){return x.endpoint==endpoint;});
    if(n==nodes_.end())return; // A result for a replaced signing key cannot revive that node.
    if(sample.health.relay_id!=endpoint.id || sample.health.authority!=authority_ || sample.health.capacity!=endpoint.capacity ||
       sample.health.active_connections>endpoint.capacity || sample.rtt.count()<0 || sample.rtt>std::chrono::seconds(10))
        throw std::invalid_argument("relay sample identity or bounds mismatch");
    n->sample=sample;n->observed=now;n->failures=0;n->retry={};
}
void RelayPool::failed(const std::string& id,Clock::time_point now) {
    for(auto& n:nodes_)if(n.endpoint.id==id) {
        n.sample.reset();n.failures=(std::min)(n.failures+1,6U);
        n.retry=now+std::chrono::seconds((std::min)(1U<<(n.failures-1),30U));
    }
}
void RelayPool::activate(const std::string& id,Clock::time_point now) {
    if(active_!=id){active_=id;activated_=now;}
}
std::optional<auth::RelayEndpoint> RelayPool::choose(Clock::time_point now) const {
    const Node* best=nullptr;const Node* active=nullptr;
    const auto score=[](const Node& n){return static_cast<std::uint64_t>(n.sample->rtt.count())+n.sample->health.upstream_rtt_us+
        std::uint64_t{15'000}*n.sample->health.active_connections/n.endpoint.capacity;};
    for(const auto& n:nodes_) {
        if(!n.sample || !n.sample->health.ready || n.sample->health.active_connections>=n.endpoint.capacity || now<n.retry ||
           now-n.observed>=std::chrono::seconds(5))continue;
        if(n.endpoint.id==active_)active=&n;
        if(!best || score(n)<score(*best) || (score(n)==score(*best) && n.endpoint.id<best->endpoint.id))best=&n;
    }
    if(active && best && (now-activated_<std::chrono::seconds(30) || score(*best)*4>=score(*active)*3))best=active;
    return best?std::optional(best->endpoint):std::nullopt;
}
}
