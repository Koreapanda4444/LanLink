#pragma once
#include <algorithm>
#include <chrono>
namespace lanlink::p2p {
class RevalidationBackoff {
public:
    using Clock=std::chrono::steady_clock;
    void failed(Clock::time_point now=Clock::now()) noexcept {
        if(pending_)return;
        next_=now+std::chrono::milliseconds{std::min(1000U*(1U<<std::min(failures_,5U)),30000U)};
        failures_=std::min(failures_+1,6U);pending_=true;
    }
    void started()noexcept{pending_=false;}
    void healthy()noexcept{pending_=false;failures_=0;}
    [[nodiscard]] bool pending()const noexcept{return pending_;}
    [[nodiscard]] bool due(Clock::time_point now=Clock::now())const noexcept{return pending_ && now>=next_;}
    [[nodiscard]] Clock::time_point next()const noexcept{return next_;}
private:
    unsigned failures_=0;bool pending_=false;Clock::time_point next_{};
};
}
