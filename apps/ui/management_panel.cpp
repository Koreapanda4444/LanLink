#include "management_panel.hpp"
#include <imgui.h>
#include <algorithm>
#include <ctime>
#include <cstdio>
namespace lanlink::ui {
namespace {
using namespace protocol;
std::string hex(const auth::DeviceId& id) {constexpr char digits[]="0123456789abcdef";std::string s;for(auto b:id){const auto n=std::to_integer<unsigned>(b);s+=digits[n>>4];s+=digits[n&15];}return s;}
std::string ip(std::uint32_t n){return std::to_string(n>>24)+"."+std::to_string((n>>16)&255)+"."+std::to_string((n>>8)&255)+"."+std::to_string(n&255);}
std::string timestamp(std::uint64_t value) {const auto seconds=static_cast<std::time_t>(value/1000);std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm,&seconds);
#else
    gmtime_r(&seconds,&tm);
#endif
    char text[32]{};std::strftime(text,sizeof(text),"%m-%d %H:%M:%S",&tm);return text;
}
}
void ManagementPanel::draw(NetworkModel& model,const NetworkSnapshot& state,const protocol::NetworkSummary& selected,core::UiLanguage language,bool force_open) {
    using namespace protocol;
    const auto tr=[&](const char* s){return core::localized_text(language,s).data();};
    const auto now=std::chrono::steady_clock::now();const auto own=state.activity.device_id;
    const bool connected=state.diagnostics && state.diagnostics->authenticated;
    if(force_open && !forced_){open_=true;forced_=true;}
    if(ImGui::Button(tr("Network management")))open_=true;
    if(open_){ImGui::OpenPopup("Network management###management");open_=false;refresh_={};}
    const auto size=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2((std::min)(820.0f,size.x-40),(std::min)(650.0f,size.y-60)),ImGuiCond_FirstUseEver);
    const auto title=std::string(tr("Network management"))+"###management";
    if(!ImGui::BeginPopupModal(title.c_str(),nullptr,ImGuiWindowFlags_NoSavedSettings))return;
    if(network_!=selected.network_id){network_=selected.network_id;draft_={};dirty_=false;generation_=0;refresh_={};route_gateway_={};error_.clear();}
    const auto inspect=[&]{ManagementRequest q;q.network_id=network_;model.submit(LocalCommand::network_management,encode_management_request(q));};
    if(connected && now>=refresh_){if(state.management_supported)inspect();model.submit(LocalCommand::routing_options);refresh_=now+std::chrono::seconds(2);}
    const auto* current=state.management && state.management->network_id==network_ && state.management->code==NetworkResultCode::success ? &*state.management : nullptr;
    if(current && generation_!=state.management_generation){generation_=state.management_generation;if(!dirty_ || (awaiting_ && current->action==ManagementAction::replace_policy)){draft_=current->policy;dirty_=false;awaiting_=false;error_.clear();}}
    if(state.routing_options && !local_dirty_)local_options_=*state.routing_options;
    const bool owner=own==selected.owner_device_id;
    const bool manager=owner || (current && current->policy.managed && policy_role(current->policy,own)==AccessRole::administrator);
    const bool fresh=current && now-state.management_updated_at<std::chrono::seconds(6) && state.peers && state.peers->network_id==network_ && now-state.peers_updated_at<std::chrono::seconds(6);
    const bool stale=current && draft_.revision!=current->policy.revision;
    ImGui::TextUnformatted(selected.name.c_str());
    ImGui::SameLine();if(ImGui::SmallButton(tr("Refresh settings")))inspect();
    if(!state.management_supported)ImGui::TextWrapped("%s",tr("Management unavailable. Update the client and relay, then reconnect."));
    else if(!current)ImGui::TextWrapped("%s",tr("Waiting for network settings."));
    if(current)ImGui::TextDisabled(tr("Policy revision: %llu / %s"),static_cast<unsigned long long>(current->policy.revision),manager?tr("Administrator"):tr("Read only"));
    const auto* peers=state.peers && state.peers->network_id==network_?&*state.peers:nullptr;
    std::vector<auth::DeviceId> devices;if(own!=auth::DeviceId{})devices.push_back(own);if(peers)for(const auto& peer:peers->peers)devices.push_back(peer.device_id);
    const auto device_label=[&](const auth::DeviceId& device){std::string label=hex(device).substr(0,12);if(device==own)label=std::string(tr("This device"))+" / "+label;else if(peers)for(const auto& peer:peers->peers)if(peer.device_id==device)label=ip(peer.ipv4_address)+" / "+label;return label;};
    const auto subject_label=[&](const PolicySubject& subject){return subject.device!=auth::DeviceId{}?device_label(subject.device):subject.group.empty()?std::string(tr("All members")):subject.group;};
    const auto subject_combo=[&](const char* id,PolicySubject& subject){if(ImGui::BeginCombo(id,subject_label(subject).c_str())){if(ImGui::Selectable(tr("All members"),subject==PolicySubject{}))subject={};for(const auto& group:std::vector<std::string>{"members"})if(ImGui::Selectable(group.c_str(),subject.group==group))subject={{},group};for(const auto& group:draft_.groups)if(ImGui::Selectable(group.c_str(),subject.group==group))subject={{},group};for(const auto& device:devices)if(ImGui::Selectable(device_label(device).c_str(),subject.device==device))subject={device,{}};ImGui::EndCombo();}};
    const auto group_combo=[&](const char* id,std::string& value,bool any){bool changed=false;if(ImGui::BeginCombo(id,value.empty()?tr("All permitted groups"):value.c_str())){if(any && ImGui::Selectable(tr("All permitted groups"),value.empty())){value.clear();changed=true;}for(const auto& group:std::vector<std::string>{"members"})if(ImGui::Selectable(group.c_str(),value==group)){value=group;changed=true;}for(const auto& group:draft_.groups)if(ImGui::Selectable(group.c_str(),value==group)){value=group;changed=true;}ImGui::EndCombo();}return changed;};
    bool show_preview=false,show_local=false;
    ImGui::BeginChild("management-body",ImVec2(0,-100),ImGuiChildFlags_Borders);
    if(ImGui::BeginTabBar("management-tabs")) {
        if(ImGui::BeginTabItem(tr("Topology"))) {
            ImGui::TextWrapped("%s",tr("Managed networks use default deny. A matching allow is required; every matching deny takes priority."));
            ImGui::BeginDisabled(!manager || !current);
            bool managed=draft_.managed;
            ImGui::BeginDisabled(!owner);
            if(ImGui::Checkbox(tr("Enable managed policy"),&managed)){const auto revision=draft_.revision;draft_={};draft_.revision=revision;draft_.managed=managed;dirty_=true;}
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!draft_.managed);
            const char* topology=draft_.topology==NetworkTopology::mesh?tr("Mesh"):tr("Hub and spoke");
            if(ImGui::BeginCombo(tr("Topology"),topology)){for(const auto choice:{NetworkTopology::mesh,NetworkTopology::hub_spoke})if(ImGui::Selectable(choice==NetworkTopology::mesh?tr("Mesh"):tr("Hub and spoke"),draft_.topology==choice)){draft_.topology=choice;if(choice==NetworkTopology::mesh)draft_.hubs.clear();dirty_=true;}ImGui::EndCombo();}
            if(draft_.topology==NetworkTopology::hub_spoke){ImGui::TextWrapped("%s",tr("Select hubs. Two spokes cannot communicate, even with an allow rule or an offline hub."));for(const auto& d:devices){bool hub=std::find(draft_.hubs.begin(),draft_.hubs.end(),d)!=draft_.hubs.end();if(ImGui::Checkbox(device_label(d).c_str(),&hub)){if(hub)draft_.hubs.push_back(d);else std::erase(draft_.hubs,d);dirty_=true;}}}
            ImGui::EndDisabled();ImGui::EndDisabled();
            ImGui::SeparatorText(tr("This device"));
            ImGui::TextWrapped("%s",tr("Local routing is optional and applies to approved networks on this device until the service restarts."));
            ImGui::BeginDisabled(!connected || !state.routing_options);
            if(ImGui::Checkbox(tr("Accept approved private subnet routes"),&local_options_[0]))local_dirty_=true;
            if(ImGui::Checkbox(tr("Forward approved gateway traffic"),&local_options_[1]))local_dirty_=true;
            ImGui::BeginDisabled(!local_dirty_ || state.busy);if(ImGui::Button(tr("Review local routing")))show_local=true;ImGui::EndDisabled();ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(tr("Access rules"))) {
            ImGui::TextWrapped("%s",tr("Rules permit or deny device pairs and groups in both directions. Hub restrictions also apply."));
            ImGui::BeginDisabled(!manager || !draft_.managed);
            for(std::size_t i=0;i<draft_.rules.size();++i){auto& rule=draft_.rules[i];ImGui::PushID(static_cast<int>(i));bool allow=rule.allow;if(ImGui::Checkbox(tr("Allow"),&allow)){rule.allow=allow;dirty_=true;}ImGui::SameLine();ImGui::TextWrapped("%s / %s",subject_label(rule.first).c_str(),subject_label(rule.second).c_str());ImGui::SameLine();if(ImGui::SmallButton(tr("Remove rule"))){draft_.rules.erase(draft_.rules.begin()+static_cast<std::ptrdiff_t>(i));dirty_=true;ImGui::PopID();break;}ImGui::PopID();}
            ImGui::SetNextItemWidth(-1);subject_combo("##first-subject",first_);ImGui::SetNextItemWidth(-1);subject_combo("##second-subject",second_);
            ImGui::BeginDisabled(draft_.rules.size()>=64);if(ImGui::Button(tr("Add allow rule"))){draft_.rules.push_back({first_,second_,true});dirty_=true;}ImGui::SameLine();if(ImGui::Button(tr("Add deny rule"))){draft_.rules.push_back({first_,second_,false});dirty_=true;}ImGui::EndDisabled();
            ImGui::EndDisabled();ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(tr("Groups and roles"))) {
            ImGui::TextWrapped("%s",tr("Only the owner assigns roles. Restricted members cannot use gateways or discovery fanout; their restriction survives rejoining."));
            ImGui::BeginDisabled(!manager || !draft_.managed);
            for(std::size_t i=0;i<draft_.groups.size();++i){const auto group=draft_.groups[i];bool used=false;for(const auto& m:draft_.members)used|=m.group==group;for(const auto& r:draft_.rules)used|=r.first.group==group || r.second.group==group;for(const auto& r:draft_.routes)used|=r.group==group;ImGui::PushID(static_cast<int>(i));ImGui::TextUnformatted(group.c_str());ImGui::SameLine();ImGui::BeginDisabled(used);if(ImGui::SmallButton(tr("Remove group"))){draft_.groups.erase(draft_.groups.begin()+static_cast<std::ptrdiff_t>(i));dirty_=true;ImGui::EndDisabled();ImGui::PopID();break;}ImGui::EndDisabled();ImGui::PopID();}
            ImGui::InputTextWithHint("##group-name",tr("Group name (letters, numbers, _ or -)"),group_.data(),group_.size());ImGui::SameLine();ImGui::BeginDisabled(draft_.groups.size()>=16 || !group_[0]);if(ImGui::Button(tr("Add group"))){draft_.groups.emplace_back(group_.data());dirty_=true;group_.fill(0);}ImGui::EndDisabled();
            auto listed=devices;for(const auto& m:draft_.members)if(std::find(listed.begin(),listed.end(),m.device)==listed.end())listed.push_back(m.device);
            for(const auto& d:listed){ImGui::PushID(hex(d).c_str());ImGui::Separator();ImGui::TextUnformatted(device_label(d).c_str());
                auto role=policy_role(draft_,d);std::string group(policy_group(draft_,d));bool changed=false;
                ImGui::BeginDisabled(!owner || d==selected.owner_device_id);
                const auto role_name=[&](AccessRole r){return r==AccessRole::administrator?tr("Administrator"):r==AccessRole::restricted?tr("Restricted member"):tr("Member");};
                ImGui::SetNextItemWidth(210);if(ImGui::BeginCombo("##member-role",d==selected.owner_device_id?tr("Owner"):role_name(role))){for(const auto r:{AccessRole::member,AccessRole::administrator,AccessRole::restricted})if(ImGui::Selectable(role_name(r),role==r)){role=r;changed=true;}ImGui::EndCombo();}ImGui::EndDisabled();
                ImGui::SameLine();ImGui::SetNextItemWidth(200);changed|=group_combo("##member-group",group,false);
                if(changed){auto found=std::find_if(draft_.members.begin(),draft_.members.end(),[&](const auto& m){return m.device==d;});if(found==draft_.members.end())draft_.members.push_back({d,role,group});else{found->role=role;found->group=group;}dirty_=true;}
                if(owner && std::any_of(draft_.members.begin(),draft_.members.end(),[&](const auto& m){return m.device==d;})){ImGui::SameLine();if(ImGui::SmallButton(tr("Clear assignment"))){std::erase_if(draft_.members,[&](const auto& m){return m.device==d;});dirty_=true;}}
                ImGui::PopID();
            }
            ImGui::EndDisabled();ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(tr("Gateways and routes"))) {
            ImGui::TextWrapped("%s",tr("A device requests gateway status before an administrator approves it. Approval alone does not enable routing."));
            ImGui::BeginDisabled(!connected || state.busy || !current || !current->policy.managed || policy_role(current->policy,own)==AccessRole::restricted);
            if(ImGui::Button(tr("Request gateway for this device"))){ManagementRequest q;q.network_id=network_;q.action=ManagementAction::request_gateway;q.policy.revision=current->policy.revision;model.submit(LocalCommand::network_management,encode_management_request(q));}ImGui::EndDisabled();
            ImGui::BeginDisabled(!manager || !draft_.managed);
            for(std::size_t i=0;i<draft_.gateways.size();++i){auto& g=draft_.gateways[i];ImGui::PushID(static_cast<int>(i));ImGui::TextUnformatted(device_label(g.device).c_str());ImGui::SameLine();bool approved=g.approved;if(ImGui::Checkbox(tr("Approved"),&approved)){g.approved=approved;if(!approved)std::erase_if(draft_.routes,[&](const auto& r){return r.gateway==g.device;});dirty_=true;}ImGui::SameLine();if(ImGui::SmallButton(tr("Revoke gateway"))){const auto device=g.device;std::erase_if(draft_.routes,[&](const auto& r){return r.gateway==device;});draft_.gateways.erase(draft_.gateways.begin()+static_cast<std::ptrdiff_t>(i));dirty_=true;ImGui::PopID();break;}ImGui::PopID();}
            ImGui::SeparatorText(tr("Routed private subnets"));
            for(std::size_t i=0;i<draft_.routes.size();++i){const auto route=draft_.routes[i];ImGui::PushID(static_cast<int>(i));ImGui::TextWrapped("%s/%u / %s / %s",ip(route.address).c_str(),static_cast<unsigned>(route.prefix),device_label(route.gateway).c_str(),route.group.empty()?tr("All permitted groups"):route.group.c_str());ImGui::SameLine();if(ImGui::SmallButton(tr("Remove route"))){draft_.routes.erase(draft_.routes.begin()+static_cast<std::ptrdiff_t>(i));dirty_=true;ImGui::PopID();break;}ImGui::PopID();}
            ImGui::SetNextItemWidth(-1);if(ImGui::BeginCombo(tr("Gateway"),route_gateway_==auth::DeviceId{}?tr("Select approved gateway"):device_label(route_gateway_).c_str())){for(const auto& g:draft_.gateways)if(g.approved && ImGui::Selectable(device_label(g.device).c_str(),route_gateway_==g.device))route_gateway_=g.device;ImGui::EndCombo();}
            ImGui::InputTextWithHint("##private-cidr","192.168.50.0/24",subnet_.data(),subnet_.size());ImGui::SetNextItemWidth(-1);group_combo(tr("Allowed group"),route_group_,true);
            ImGui::BeginDisabled(draft_.routes.size()>=32 || !subnet_[0] || route_gateway_==auth::DeviceId{});if(ImGui::Button(tr("Add private subnet"))){try{const auto [address,prefix]=parse_private_cidr(subnet_.data());draft_.routes.push_back({route_gateway_,address,prefix,route_group_});dirty_=true;error_.clear();}catch(const std::exception& e){error_=e.what();}}ImGui::EndDisabled();
            ImGui::EndDisabled();ImGui::TextWrapped("%s",tr("Only private subnets are supported. LAN return routes and forwarding must be configured locally. Internet default routes and DNS are unchanged."));ImGui::EndTabItem();
        }
        if(ImGui::BeginTabItem(tr("Audit and approvals"))) {
            if(manager && current) {
                for(const auto& d:current->pending_requests){ImGui::PushID(hex(d).c_str());ImGui::TextUnformatted(device_label(d).c_str());ImGui::SameLine();ImGui::BeginDisabled(!connected || state.busy);if(ImGui::SmallButton(tr("Approve")))model.submit(LocalCommand::approve,encode_network_member_request({network_,d}));ImGui::EndDisabled();ImGui::PopID();}
                if(current->audit_write_failures)ImGui::TextWrapped(tr("Audit write failures in this relay session: %llu"),static_cast<unsigned long long>(current->audit_write_failures));
                ImGui::TextWrapped("%s",tr("Recent history: up to 64 events. The relay retains at most 256 per network for 30 days. No secrets or packet contents are recorded."));
                if(ImGui::BeginTable("audit-events",3,ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_RowBg)) {ImGui::TableSetupColumn(tr("Time (UTC)"),ImGuiTableColumnFlags_WidthFixed,125);ImGui::TableSetupColumn(tr("Action"));ImGui::TableSetupColumn(tr("Result"),ImGuiTableColumnFlags_WidthFixed,130);ImGui::TableHeadersRow();for(const auto& event:current->audit_events){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted(timestamp(event.timestamp_ms).c_str());ImGui::TableNextColumn();ImGui::TextWrapped("%s",tr(audit_kind_name(event.kind).data()));if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",hex(event.actor).c_str());ImGui::TableNextColumn();ImGui::TextUnformatted(tr(network_result_code_name(event.result).data()));}ImGui::EndTable();}
            } else ImGui::TextWrapped("%s",tr("Audit history and join approvals are available to network administrators."));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    if(stale)ImGui::TextWrapped("%s",tr("Settings changed on the relay. Reload before applying your draft."));
    ImGui::BeginDisabled(!current);if(ImGui::Button(tr("Reload draft"))){draft_=current->policy;dirty_=false;error_.clear();}ImGui::EndDisabled();ImGui::SameLine();
    ImGui::BeginDisabled(!manager || !dirty_ || stale || !fresh || !connected || state.busy);
    if(ImGui::Button(tr("Preview changes"))){try{preview_=preview_network_policy(*peers,own,draft_);pending_=draft_;show_preview=true;error_.clear();}catch(const std::exception& e){error_=e.what();}}ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button(tr("Close")))ImGui::CloseCurrentPopup();
    if(state.management && state.management->network_id==network_ && state.management->code!=NetworkResultCode::success)ImGui::TextWrapped("%s",tr(network_result_code_name(state.management->code).data()));
    if(!error_.empty())ImGui::TextWrapped("%s",tr(error_.c_str()));
    if(show_preview)ImGui::OpenPopup("Confirm policy###apply-policy");
    const auto confirm_title=std::string(tr("Confirm policy"))+"###apply-policy";
    if(ImGui::BeginPopupModal(confirm_title.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s",tr("Applying this policy rotates network keys and closes existing direct sessions. Older clients cannot use managed networks."));
        ImGui::Text(tr("Allowed pairs: %u / Blocked pairs: %u"),preview_.allowed_pairs,preview_.blocked_pairs);
        ImGui::Text(tr("Approved gateways: %u / Routes: %u / Role changes: %u"),preview_.gateways,preview_.routes,preview_.changed_roles);
        ImGui::TextWrapped("%s",tr("Check the allowed pairs and routes. A policy with no matching allow blocks all member traffic."));
        ImGui::BeginDisabled(!fresh || stale || state.busy);if(ImGui::Button(tr("Apply policy"))){ManagementRequest q;q.action=ManagementAction::replace_policy;q.network_id=network_;q.policy=pending_;model.submit(LocalCommand::network_management,encode_management_request(q));awaiting_=true;ImGui::CloseCurrentPopup();}ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button(tr("Cancel")))ImGui::CloseCurrentPopup();ImGui::EndPopup();
    }
    if(show_local)ImGui::OpenPopup("Local routing###local-routing");
    const auto routing_title=std::string(tr("Local routing"))+"###local-routing";
    if(ImGui::BeginPopupModal(routing_title.c_str(),nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s",tr("Allowing private routes changes how this PC reaches approved LAN subnets. Gateway forwarding exposes approved LAN hosts to permitted members."));
        ImGui::TextWrapped("%s",tr("These options apply to approved networks on this PC until service restart. LAN forwarding and return routes require local administrator setup."));
        ImGui::BeginDisabled(!connected || state.busy);if(ImGui::Button(tr("Apply local routing"))){model.submit(LocalCommand::routing_options,{local_options_[0]?std::byte{1}:std::byte{0},local_options_[1]?std::byte{1}:std::byte{0}});local_dirty_=false;ImGui::CloseCurrentPopup();}ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button(tr("Cancel")))ImGui::CloseCurrentPopup();ImGui::EndPopup();
    }
    ImGui::EndPopup();
}
}
