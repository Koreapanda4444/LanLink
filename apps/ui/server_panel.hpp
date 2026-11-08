#pragma once
#include "network_model.hpp"
#include "design.hpp"
#include "lanlink/protocol/join_target.hpp"
#include "lanlink/protocol/network_workflow.hpp"
#include "lanlink/core/component.hpp"
#include <imgui.h>
#include <array>
#include <algorithm>
#include <cstdio>
namespace lanlink::ui {
class ServerPanel {
public:
    void invitation(std::string uri) {
        legacy_=!std::string_view(uri).starts_with("lanlink://connect/");
        protocol::ServerInvitation parsed;
        if(legacy_){const auto join=protocol::parse_join_target(uri);parsed.network_id=join.network_id;parsed.network_invitation=join.invitation;parsed.server.name="Network invitation on current server";}
        else parsed=protocol::parse_server_invitation(uri);
        invitation_=std::move(uri);target_=parsed;review_=true;trust_=false;switch_=false;error_.clear();
    }
    void open_manager() { manager_=true; }
    void add_server() {
        editing_.clear();name_.fill(0);host_.fill(0);pin_.fill(0);credential_.fill(0);code_.fill(0);
        port_=4433;edit_=true;trust_=false;remove_=false;registration_tab_=false;error_.clear();
    }
    void edit_server(const protocol::ServerProfile& p) {
        editing_=p.id;copy(name_,p.name);copy(host_,p.host);copy(pin_,p.public_key_pin);port_=p.port;
        credential_.fill(0);code_.fill(0);trust_=p.public_key_pin.empty();remove_=false;error_.clear();edit_=true;registration_tab_=false;
    }
    void paste_invitation() { paste_=true; }
    void draw(NetworkModel& model,const NetworkSnapshot& state,core::UiLanguage language) {
        const auto t=[&](const char* en,const char* ko){return language==core::UiLanguage::korean?ko:en;};
        const auto send=[&](protocol::ServerRequest r){model.submit(protocol::LocalCommand::server_profiles,protocol::encode_server_request(r));};
        const auto safely=[&](auto fn){try{fn();error_.clear();}catch(const std::exception& e){error_=e.what();}};
        const auto prepare_popup=[](float width) {
            const auto size=ImGui::GetMainViewport()->WorkSize;
            width=(std::min)(width,(std::max)(240.0f,size.x-24));
            ImGui::SetNextWindowSizeConstraints(ImVec2(width,0),ImVec2(width,(std::max)(200.0f,size.y-24)));
        };
        const auto manager_title=std::string(t("Servers","서버"))+"###server-manager";
        const auto settings_title=std::string(t("Server settings","서버 설정"))+"###server-settings";
        const auto invitation_title=std::string(t("Invitation","초대"))+"###server-invitation";
        if(state.servers) {
            const auto& s=*state.servers;
            if(await_saved_ && state.server_operation_generation>saved_generation_) {
                await_saved_=false;
                const auto found=std::find_if(s.profiles.begin(),s.profiles.end(),[&](const auto& p){return state.server_action==protocol::ServerAction::save && p.id==state.server_result_id;});
                if(found!=s.profiles.end()){editing_=found->id;registration_tab_=true;}
            }
            if(!editing_.empty() && std::none_of(s.profiles.begin(),s.profiles.end(),[&](const auto& p){return p.id==editing_;}))editing_.clear();
        }
        if(manager_){ImGui::OpenPopup(manager_title.c_str());manager_=false;}
        prepare_popup(520);
        bool manager_visible=true;
        if(ImGui::BeginPopupModal(manager_title.c_str(),&manager_visible,ImGuiWindowFlags_AlwaysAutoResize)) {
            if(ImGui::BeginChild("server-list",ImVec2(0,180),ImGuiChildFlags_Borders)) {
                if(state.servers)for(const auto& p:state.servers->profiles) {
                    ImGui::PushID(p.id.c_str());
                    if(ImGui::Selectable(p.name.c_str(),editing_==p.id)) {
                        editing_=p.id;copy(name_,p.name);copy(host_,p.host);copy(pin_,p.public_key_pin);port_=p.port;
                        credential_.fill(0);code_.fill(0);trust_=p.public_key_pin.empty();remove_=false;error_.clear();
                    }
                    ImGui::TextDisabled("%s",p.has_credential?t("Registered","등록됨"):t("Registration needed","기기 등록 필요"));
                    if(ImGui::IsItemHovered())ImGui::SetTooltip("%s:%u",p.host.c_str(),p.port);
                    ImGui::PopID();
                }
                if(!state.servers || state.servers->profiles.empty())ImGui::TextDisabled("%s",t("No saved servers","저장된 서버 없음"));
            }
            ImGui::EndChild();
            if(ImGui::Button(t("Add server","서버 추가"))){add_server();ImGui::CloseCurrentPopup();}
            ImGui::SameLine();ImGui::BeginDisabled(editing_.empty());
            if(ImGui::Button(t("Edit / register","수정 · 기기 등록"))){edit_=true;ImGui::CloseCurrentPopup();}
            ImGui::EndDisabled();
            bool registered=false;if(state.servers)for(const auto& p:state.servers->profiles)if(p.id==editing_)registered=p.has_credential;
            ImGui::SameLine();ImGui::BeginDisabled(state.busy || !registered);
            if(ImGui::Button(t("Connect","연결")))safely([&]{protocol::ServerRequest r;r.action=protocol::ServerAction::connect;r.profile.id=editing_;send(r);ImGui::CloseCurrentPopup();});
            ImGui::EndDisabled();
            if(ImGui::Button(t("Use invitation","초대 사용"))){paste_=true;ImGui::CloseCurrentPopup();}
            ImGui::SameLine();if(ImGui::Button(t("Close","닫기")))ImGui::CloseCurrentPopup();
            if(!state.server_error.empty())ImGui::TextWrapped("%s",core::localized_text(language,state.server_error).data());
            if(!error_.empty())ImGui::TextWrapped("%s",error_.c_str());
            ImGui::EndPopup();
        }
        const auto paste_title=std::string(t("Use invitation","초대 사용"))+"###paste-invitation";
        if(paste_){ImGui::OpenPopup(paste_title.c_str());paste_=false;}
        prepare_popup(500);
        bool paste_visible=true;
        if(ImGui::BeginPopupModal(paste_title.c_str(),&paste_visible,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(t("Invitation link or code","초대 링크 또는 코드"));
            ImGui::SetNextItemWidth(-1);ImGui::InputText("##serverinvite",uri_.data(),uri_.size());
            ImGui::BeginDisabled(!uri_[0]);
            if(ImGui::Button(t("Review invitation","초대 확인")))safely([&]{invitation(uri_.data());ImGui::CloseCurrentPopup();});
            ImGui::EndDisabled();ImGui::SameLine();
            if(ImGui::Button(t("Cancel","취소")))ImGui::CloseCurrentPopup();
            if(!error_.empty())ImGui::TextWrapped("%s",error_.c_str());
            ImGui::EndPopup();
        }
        if(edit_){ImGui::OpenPopup(settings_title.c_str());edit_=false;}
        prepare_popup(520);
        bool settings_visible=true;
        if(ImGui::BeginPopupModal(settings_title.c_str(),&settings_visible,ImGuiWindowFlags_AlwaysAutoResize)) {
            bool saved=false,registered=false;
            if(state.servers)for(const auto& p:state.servers->profiles)if(p.id==editing_) {
                saved=credential_[0]==0 && p.name==name_.data() && p.host==host_.data() && p.port==port_ && p.public_key_pin==pin_.data();
                registered=p.has_credential;
            }
            if(ImGui::BeginTabBar("server-settings-tabs")) {
                if(ImGui::BeginTabItem(t("Server","서버"))) {
                    ImGui::TextUnformatted(t("Server name","서버 이름"));
                    ImGui::SetNextItemWidth(-1);ImGui::InputText("##name",name_.data(),name_.size());
                    if(ImGui::BeginTable("server-address",2,ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("host",ImGuiTableColumnFlags_WidthStretch);ImGui::TableSetupColumn("port",ImGuiTableColumnFlags_WidthFixed,90);
                        ImGui::TableNextRow();ImGui::TableNextColumn();
                        ImGui::TextUnformatted(t("Host / IP","호스트 / IP"));ImGui::SetNextItemWidth(-1);
                        ImGui::InputText("##host",host_.data(),host_.size());
                        ImGui::TableNextColumn();ImGui::TextUnformatted(t("Port","포트"));ImGui::SetNextItemWidth(-1);
                        ImGui::InputInt("##port",&port_,0,0);ImGui::EndTable();
                    }
                    ImGui::TextUnformatted(t("Certificate fingerprint (optional)","인증서 지문 (선택)"));
                    ImGui::SetNextItemWidth(-1);ImGui::InputText("##pin",pin_.data(),pin_.size());
                    ImGui::TextWrapped("%s",t("Verify the fingerprint with the operator. Changing the address or fingerprint resets device registration.","운영자에게 지문을 확인하세요. 주소나 지문을 바꾸면 기기 등록이 초기화됩니다."));
                    if(pin_[0])ImGui::Checkbox(t("I verified this server fingerprint","서버 공개키 지문을 확인했습니다"),&trust_);
                    if(ImGui::CollapsingHeader(t("Existing individual credential (optional)","기존 개별 인증 정보 (선택)"))) {
                        ImGui::SetNextItemWidth(-1);ImGui::InputText("##individualcredential",credential_.data(),credential_.size(),ImGuiInputTextFlags_Password);
                        ImGui::TextWrapped("%s",t("Leave blank to keep a saved credential. Never enter the operator's master token.","비워 두면 저장된 인증 정보가 유지됩니다. 운영자의 관리용 토큰을 입력하지 마세요."));
                    }
                    ImGui::BeginDisabled(state.busy || !name_[0] || !host_[0] || port_<1 || port_>65535 || (pin_[0] && !trust_));
                    if(design::button(t("Save server","서버 저장"),ImVec2(0,34),true))safely([&]{
                        protocol::ServerRequest r;r.action=protocol::ServerAction::save;
                        r.profile={editing_,name_.data(),host_.data(),static_cast<std::uint16_t>(port_),pin_.data()};r.credential=credential_.data();
                        protocol::validate_server_profile(r.profile);saved_generation_=state.server_operation_generation;await_saved_=true;
                        send(r);credential_.fill(0);
                    });
                    ImGui::EndDisabled();
                    if(!editing_.empty() && ImGui::CollapsingHeader(t("Remove server","서버 삭제"))) {
                        ImGui::BeginDisabled(state.busy);
                        ImGui::Checkbox(t("Confirm deletion of selected profile","선택한 서버 프로필 삭제 확인"),&remove_);
                        if(remove_ && ImGui::Button(t("Delete server","서버 삭제")))safely([&]{protocol::ServerRequest r;r.action=protocol::ServerAction::remove;r.profile.id=editing_;send(r);editing_.clear();remove_=false;ImGui::CloseCurrentPopup();});
                        ImGui::EndDisabled();
                    }
                    ImGui::EndTabItem();
                }
                if(ImGui::BeginTabItem(t("Device","기기"),nullptr,registration_tab_?ImGuiTabItemFlags_SetSelected:0)) {
                    registration_tab_=false;
                    if(!saved)ImGui::TextWrapped("%s",t("Save changes before registering or connecting.","기기를 등록하거나 연결하기 전에 변경 내용을 저장하세요."));
                    if(registered) {
                        design::status(t("Registered","등록됨"),true);ImGui::Spacing();
                    } else {
                        ImGui::TextWrapped("%s",t("Enter a one-use code from the server operator.","서버 운영자가 발급한 일회용 코드를 입력하세요."));
                        ImGui::TextUnformatted(t("Registration code","등록 코드"));ImGui::SetNextItemWidth(-1);
                        ImGui::InputText("##registration-code",code_.data(),code_.size(),ImGuiInputTextFlags_Password);
                        ImGui::TextUnformatted(t("Device label","기기 이름"));ImGui::SetNextItemWidth(-1);
                        ImGui::InputText("##device-label",label_.data(),label_.size());
                        ImGui::BeginDisabled(state.busy || !saved || editing_.empty() || !code_[0] || !label_[0]);
                        if(design::button(t("Register device","기기 등록"),ImVec2(0,34),true))safely([&]{protocol::ServerRequest r;r.action=protocol::ServerAction::enroll;r.profile.id=editing_;r.registration_code=code_.data();r.device_label=label_.data();send(r);code_.fill(0);});
                        ImGui::EndDisabled();
                    }
                    ImGui::BeginDisabled(state.busy || !saved || !registered);
                    if(design::button(t("Connect","연결"),ImVec2(0,34),true))safely([&]{protocol::ServerRequest r;r.action=protocol::ServerAction::connect;r.profile.id=editing_;send(r);ImGui::CloseCurrentPopup();});
                    ImGui::EndDisabled();ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::Separator();
            if(ImGui::Button(t("Close","닫기"))){credential_.fill(0);code_.fill(0);remove_=false;ImGui::CloseCurrentPopup();}
            if(!state.server_error.empty())ImGui::TextWrapped("%s",core::localized_text(language,state.server_error).data());
            if(!error_.empty())ImGui::TextWrapped("%s",error_.c_str());
            ImGui::EndPopup();
        }
        if(!settings_visible){credential_.fill(0);code_.fill(0);remove_=false;}
        if(review_){ImGui::OpenPopup(invitation_title.c_str());review_=false;}
        prepare_popup(520);
        bool review_visible=true;
        if(ImGui::BeginPopupModal(invitation_title.c_str(),&review_visible,ImGuiWindowFlags_AlwaysAutoResize)) {
            if(legacy_) {
                ImGui::TextUnformatted(t("Review network invitation","네트워크 초대 확인"));
                ImGui::TextUnformatted(t("Join using the currently connected server.","현재 연결된 서버에서 네트워크에 가입합니다."));
                if(state.servers)for(const auto& p:state.servers->profiles)if(p.id==state.servers->selected_id)
                    ImGui::TextWrapped("%s / %s:%u",p.name.c_str(),p.host.c_str(),p.port);
            } else {
                ImGui::TextUnformatted(t("Review server and network invitation","서버 및 네트워크 초대 확인"));
                ImGui::TextWrapped("%s\n%s:%u",target_.server.name.c_str(),target_.server.host.c_str(),target_.server.port);
                ImGui::TextWrapped("%s: %s",t("Server public key","서버 공개키"),target_.server.public_key_pin.empty()?t("Normal certificate validation","일반 인증서 검증"):target_.server.public_key_pin.c_str());
                if(!target_.server.public_key_pin.empty())ImGui::Checkbox(t("I verified this server fingerprint","서버 공개키 지문을 확인했습니다"),&trust_);
                ImGui::Checkbox(t("Approve switching to this server","이 서버로 연결 전환 승인"),&switch_);
            }
            std::string network;for(auto b:target_.network_id){const auto n=std::to_integer<unsigned>(b);network+="0123456789abcdef"[n>>4];network+="0123456789abcdef"[n&15];}
            ImGui::TextWrapped("%s: %s",t("Target network","대상 네트워크"),network==std::string(32,'0')?t("None","없음"):network.c_str());
            ImGui::TextWrapped("%s",t("Network access still requires the network owner's permission.","네트워크 가입에는 해당 네트워크의 권한이 필요합니다."));
            ImGui::BeginDisabled(state.busy || (legacy_ ? (!state.service_online || !state.diagnostics || !state.diagnostics->authenticated) : (!state.servers || (!target_.server.public_key_pin.empty() && !trust_) || !switch_)));
            if(design::button(t("Accept invitation","초대 수락"),ImVec2(0,34),true))safely([&]{if(legacy_){const auto q=protocol::join_network_command(invitation_,"");model.submit(q.command,q.payload);invitation_.clear();ImGui::CloseCurrentPopup();return;}protocol::ServerRequest r;r.action=protocol::ServerAction::accept_invitation;r.expected_epoch=state.servers->connection_epoch;r.invitation_uri=invitation_;r.approve_server_switch=switch_;r.trust_invitation_pin=trust_;r.device_label=label_[0]?label_.data():"LanLink device";send(r);invitation_.clear();uri_.fill(0);ImGui::CloseCurrentPopup();});
            ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button(t("Cancel","취소"))){invitation_.clear();uri_.fill(0);ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();
        }
        if(!review_visible){invitation_.clear();uri_.fill(0);}
        if(!paste_visible)uri_.fill(0);
    }
private:
    template<std::size_t N>static void copy(std::array<char,N>& b,const std::string& s){b.fill(0);std::copy_n(s.data(),std::min(s.size(),N-1),b.data());}
    std::string editing_,invitation_,error_;
    protocol::ServerInvitation target_;
    std::array<char,129> name_{},label_{};
    std::array<char,254> host_{};
    std::array<char,65> pin_{};
    std::array<char,4097> credential_{};
    std::array<char,41> code_{};
    std::array<char,2049> uri_{};
    std::uint64_t saved_generation_=0;
    bool await_saved_=false,registration_tab_=false;
    int port_=4433;
    bool legacy_=false,edit_=false,review_=false,trust_=false,switch_=false,remove_=false,manager_=false,paste_=false;
};
}
