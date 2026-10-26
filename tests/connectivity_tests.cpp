#include "lanlink/p2p/connectivity.hpp"
#include <iostream>
using namespace lanlink;
int main(){try {
    auth::DeviceId a{},b{};a[0]=std::byte{1};b[0]=std::byte{2};
    const std::vector<p2p::Candidate> local{{{0xc0a80101,4500},p2p::CandidateKind::host,200}},remote{
        {{0xc0a80102,4501},p2p::CandidateKind::host,200},{{0xcb007102,4502},p2p::CandidateKind::server_reflexive,100}};
    const auto now=p2p::ConnectivityChecks::Clock::now();p2p::ConnectivityChecks checks(a,b,local,remote,now),other(b,a,remote,local,now);
    checks.resolve_role_conflict(p2p::IceRole::controlling);other.resolve_role_conflict(p2p::IceRole::controlled);
    if(checks.role()!=p2p::IceRole::controlling || other.role()!=p2p::IceRole::controlled)throw std::runtime_error("role conflict was not resolved");
    const auto pair=checks.next(now);if(!pair || pair->remote.endpoint!=remote[0].endpoint)throw std::runtime_error("pair priority ignored");
    checks.result(pair->local.endpoint,pair->remote.endpoint,true,1200);
    if(!checks.nominate(now) || checks.state()!=p2p::ConnectivityState::connected)throw std::runtime_error("authenticated check was not nominated");
    p2p::ConnectivityChecks blocked(a,b,local,remote,now);unsigned attempts=0;
    for(unsigned ms=0;ms<5200;ms+=100)if(const auto p=blocked.next(now+std::chrono::milliseconds{ms})) {++attempts;blocked.result(p->local.endpoint,p->remote.endpoint,false);}
    if(blocked.state()!=p2p::ConnectivityState::failed || attempts!=6)throw std::runtime_error("blocked checks did not stop after bounded retries");
    // A response for an unprobed endpoint cannot nominate a connection.
    p2p::ConnectivityChecks spoof(a,b,local,remote,now);spoof.result(local[0].endpoint,remote[0].endpoint,true,1);
    if(spoof.nominate(now))throw std::runtime_error("unrequested result nominated a candidate");
    std::cout<<"connectivity priority, nomination, roles and timeout passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
