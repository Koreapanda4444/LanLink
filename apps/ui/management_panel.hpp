#pragma once
#include "network_model.hpp"
#include "lanlink/core/localization.hpp"
#include "lanlink/protocol/policy_preview.hpp"
#include <array>
namespace lanlink::ui {
class ManagementPanel {
public:
    void draw(NetworkModel& model,const NetworkSnapshot& state,const protocol::NetworkSummary& selected,core::UiLanguage language,bool force_open=false);
private:
    protocol::NetworkId network_{};
    protocol::NetworkPolicy draft_,pending_;
    protocol::PolicyPreview preview_;
    protocol::PolicySubject first_,second_;
    auth::DeviceId route_gateway_{};
    std::string route_group_,error_;
    std::array<char,33> group_{};
    std::array<char,40> subnet_{};
    std::array<bool,2> local_options_{};
    bool dirty_=false,local_dirty_=false,open_=false,forced_=false,awaiting_=false;
    std::uint64_t generation_=0;
    std::chrono::steady_clock::time_point refresh_{};
};
}
