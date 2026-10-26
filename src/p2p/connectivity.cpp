#include "lanlink/p2p/connectivity.hpp"
#include <algorithm>
#include <stdexcept>
namespace lanlink::p2p {
ConnectivityChecks::ConnectivityChecks(auth::DeviceId local,auth::DeviceId peer,std::vector<Candidate> locals,
    std::vector<Candidate> remotes,Clock::time_point now):local_(local),peer_(peer),role_(local<peer?IceRole::controlling:IceRole::controlled),
    deadline_(now+std::chrono::seconds{5}),next_(now) {
    if(local==peer || locals.empty() || remotes.empty() || locals.size()>max_candidates || remotes.size()>max_candidates)
        throw std::invalid_argument("invalid connectivity checklist");
    for(const auto& a:locals)for(const auto& b:remotes) {
        if(!usable_endpoint(a.endpoint) || !usable_endpoint(b.endpoint) || !a.priority || !b.priority)throw std::invalid_argument("invalid candidate pair");
        const auto g=role_==IceRole::controlling?a.priority:b.priority,d=role_==IceRole::controlling?b.priority:a.priority;
        const auto priority=(std::uint64_t{std::min(g,d)}<<32)+2ULL*std::max(g,d)+(g>d?1:0);
        pairs_.push_back({a,b,priority,CheckState::waiting,0,{}});
    }
    std::stable_sort(pairs_.begin(),pairs_.end(),[](const auto& a,const auto& b){return a.priority>b.priority;});
}
void ConnectivityChecks::resolve_role_conflict(IceRole peer_role) {
    if(role_==peer_role)role_=local_<peer_?IceRole::controlling:IceRole::controlled;
}
std::optional<CandidatePair> ConnectivityChecks::next(Clock::time_point now) {
    if(state_!=ConnectivityState::checking || now<next_)return std::nullopt;
    if(now>=deadline_) {for(auto& p:pairs_)if(p.state!=CheckState::succeeded)p.state=CheckState::failed;static_cast<void>(nominate(now));return std::nullopt;}
    auto selected=std::find_if(pairs_.begin(),pairs_.end(),[](const auto& p){return p.state==CheckState::waiting || (p.state==CheckState::in_progress && p.attempts<3);});
    if(selected==pairs_.end()) {static_cast<void>(nominate(now));return std::nullopt;}
    selected->state=CheckState::in_progress;++selected->attempts;next_=now+std::chrono::milliseconds{100};return *selected;
}
void ConnectivityChecks::result(Endpoint local,Endpoint remote,bool success,std::optional<std::uint32_t> rtt) {
    if(state_!=ConnectivityState::checking)return;
    for(auto& p:pairs_)if(p.local.endpoint==local && p.remote.endpoint==remote && p.state==CheckState::in_progress) {
        p.state=success?CheckState::succeeded:(p.attempts>=3?CheckState::failed:CheckState::waiting);p.rtt_us=success?rtt:std::nullopt;
    }
}
std::optional<CandidatePair> ConnectivityChecks::nominate(Clock::time_point now) {
    if(state_==ConnectivityState::failed)return std::nullopt;
    for(const auto& p:pairs_) {
        if(p.state==CheckState::succeeded){state_=ConnectivityState::connected;return p;}
        if(now<deadline_ && p.state!=CheckState::failed)return std::nullopt;
    }
    state_=ConnectivityState::failed;return std::nullopt;
}
}
