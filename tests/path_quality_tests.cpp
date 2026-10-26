#include "lanlink/p2p/path_quality.hpp"
#include <iostream>
using namespace lanlink::p2p;
int main(){try{
    const auto start=PathSelector::Clock::now();PathSelector selector;
    QualitySample good{true,1000,100,0};
    if(selector.sample(good,2000,start).direct)throw std::runtime_error("one sample selected an unstable path");
    static_cast<void>(selector.sample(good,2000,start+std::chrono::milliseconds{200}));
    if(!selector.sample(good,2000,start+std::chrono::milliseconds{400}).direct)throw std::runtime_error("stable direct path was not selected");
    auto bad=good;bad.rtt_us=1000000;bad.transport_sent_packets+=100;bad.lost_packets+=60;
    for(unsigned i=0;i<4;++i)static_cast<void>(selector.sample(bad,2000,start+std::chrono::milliseconds{600+i*200}));
    if(!selector.sample(bad,2000,start+std::chrono::milliseconds{1500}).direct)throw std::runtime_error("hold time did not prevent flapping");
    if(selector.sample(bad,2000,start+std::chrono::seconds{3}).direct || !selector.loss_basis_points() || *selector.loss_basis_points()!=6000)throw std::runtime_error("persistent measured degradation did not select Relay");
    for(unsigned i=0;i<80;++i){good.transport_sent_packets+=100;static_cast<void>(selector.sample(good,2000,start+std::chrono::milliseconds{4000+i*200}));}
    if(!selector.sample(good,2000,start+std::chrono::seconds{21}).direct)throw std::runtime_error("healthy path did not recover");
    if(selector.sample({},2000,start+std::chrono::seconds{21}).direct)throw std::runtime_error("dead path retained hold-time preference");
    std::cout<<"measured RTT/loss, hold time, degradation and recovery passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
