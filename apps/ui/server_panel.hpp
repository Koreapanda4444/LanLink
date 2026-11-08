#pragma once
#include "network_model.hpp"
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
    void draw(NetworkModel& model,const NetworkSnapshot& state,core::UiLanguage language) {
        const auto t=[&](const char* en,const char* ko){return language==core::UiLanguage::korean?ko:en;};
        const auto send=[&](protocol::ServerRequest r){model.submit(protocol::LocalCommand::server_profiles,protocol::encode_server_request(r));};
        const auto safely=[&](auto fn){try{fn();error_.clear();}catch(const std::exception& e){error_=e.what();}};
        ImGui::SeparatorText(t("Servers","서버"));
        if(state.servers) {
            const auto& s=*state.servers;
            if(await_saved_ && state.server_operation_generation>saved_generation_) {
                await_saved_=false;
                const auto found=std::find_if(s.profiles.begin(),s.profiles.end(),[&](const auto& p){return state.server_action==protocol::ServerAction::save && p.id==state.server_result_id;});
                if(found!=s.profiles.end()){editing_=found->id;edit_=true;await_saved_=false;}
            }
            if(s.selected_id.empty())ImGui::TextDisabled("%s",t("Disconnected","서버 미연결"));
            for(const auto& p:s.profiles) {
                ImGui::PushID(p.id.c_str());
                if(ImGui::Selectable((p.name+(p.id==s.selected_id?" *":"")).c_str(),editing_==p.id)) {
                    editing_=p.id;copy(name_,p.name);copy(host_,p.host);copy(pin_,p.public_key_pin);port_=p.port;credential_.fill(0);code_.fill(0);trust_=p.public_key_pin.empty();
                }
                ImGui::TextDisabled("%s:%u / %s",p.host.c_str(),p.port,p.has_credential?t("Registered","등록됨"):t("Registration needed","기기 등록 필요"));
                ImGui::PopID();
            }
            ImGui::BeginDisabled(state.busy || editing_.empty());
            if(ImGui::SmallButton(t("Connect","연결")))safely([&]{protocol::ServerRequest r;r.action=protocol::ServerAction::connect;r.profile.id=editing_;send(r);});
            ImGui::SameLine();if(ImGui::SmallButton(t("Disconnect","연결 해제")))safely([&]{protocol::ServerRequest r;r.action=protocol::ServerAction::disconnect;send(r);});
            ImGui::EndDisabled();
            if(!s.notice.empty())ImGui::TextWrapped("%s",core::localized_text(language,s.notice).data());
        }
        if(ImGui::SmallButton(t("Add server","서버 추가"))){editing_.clear();name_.fill(0);host_.fill(0);pin_.fill(0);credential_.fill(0);code_.fill(0);port_=4433;edit_=true;trust_=false;}
        ImGui::SameLine();ImGui::BeginDisabled(editing_.empty());
        if(ImGui::SmallButton(t("Edit / register","수정 · 기기 등록")))edit_=true;
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##serverinvite",t("Paste lanlink:// invitation","lanlink:// 초대 붙여넣기"),uri_.data(),uri_.size());
        if(ImGui::SmallButton(t("Review invitation","초대 확인")))safely([&]{invitation(uri_.data());});
        if(edit_){ImGui::OpenPopup("Server settings##server");edit_=false;}
        if(ImGui::BeginPopupModal("Server settings##server",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(t("Server settings and device registration","서버 설정 및 기기 등록"));
            ImGui::SetNextItemWidth(420);ImGui::InputText(t("Server name","서버 이름"),name_.data(),name_.size());
            ImGui::SetNextItemWidth(420);ImGui::InputText(t("Host / IP","호스트 / IP"),host_.data(),host_.size());
            ImGui::InputInt(t("Port","포트"),&port_);
            ImGui::SetNextItemWidth(420);ImGui::InputText(t("SPKI SHA-256 (optional)","서버 공개키 SHA-256 (선택)"),pin_.data(),pin_.size());
            ImGui::TextWrapped("%s",t("Ask the operator for the fingerprint using a trusted channel. An empty fingerprint uses normal certificate validation. Changing the address or fingerprint clears this profile's credentials.","운영자에게 신뢰할 수 있는 경로로 지문을 확인하세요. 비어 있으면 일반 인증서 검증을 사용합니다. 주소나 지문을 바꾸면 해당 프로필의 인증 정보가 초기화됩니다."));
            if(pin_[0])ImGui::Checkbox(t("I verified this server fingerprint","서버 공개키 지문을 확인했습니다"),&trust_);
            if(ImGui::CollapsingHeader(t("Existing individual credential (optional)","기존 개별 인증 정보 (선택)"))) {
                ImGui::SetNextItemWidth(420);ImGui::InputText("##individualcredential",credential_.data(),credential_.size(),ImGuiInputTextFlags_Password);
                ImGui::TextWrapped("%s",t("Leave blank to keep a saved credential. Never enter the operator's master token.","비워 두면 저장된 인증 정보가 유지됩니다. 운영자의 관리용 토큰을 입력하지 마세요."));
            }
            ImGui::BeginDisabled(state.busy || port_<1 || port_>65535 || (pin_[0] && !trust_));
            if(ImGui::Button(t("Save server","서버 저장")))safely([&]{protocol::ServerRequest r;r.action=protocol::ServerAction::save;r.profile={editing_,name_.data(),host_.data(),static_cast<std::uint16_t>(port_),pin_.data()};r.credential=credential_.data();protocol::validate_server_profile(r.profile);saved_generation_=state.server_operation_generation;await_saved_=editing_.empty();send(r);credential_.fill(0);ImGui::CloseCurrentPopup();});
            ImGui::EndDisabled();
            bool saved=false;
            if(state.servers && credential_[0]==0)for(const auto& p:state.servers->profiles)
                if(p.id==editing_)saved=p.name==name_.data() && p.host==host_.data() && p.port==port_ && p.public_key_pin==pin_.data();
            if(!saved)ImGui::TextWrapped("%s",t("Save changes before registering or connecting.","기기를 등록하거나 연결하기 전에 변경 내용을 저장하세요."));
            ImGui::SeparatorText(t("Register this device","이 기기 등록"));
            ImGui::TextWrapped("%s",t("Save the server first, then use a one-use code from its operator.","서버를 먼저 저장한 뒤 운영자가 발급한 일회용 등록 코드를 입력하세요."));
            ImGui::InputText(t("Registration code","등록 코드"),code_.data(),code_.size(),ImGuiInputTextFlags_Password);
            ImGui::InputText(t("Device label","기기 이름"),label_.data(),label_.size());
            ImGui::BeginDisabled(state.busy || !saved || editing_.empty() || !code_[0] || !label_[0]);
            if(ImGui::Button(t("Register device","기기 등록")))safely([&]{protocol::ServerRequest r;r.action=protocol::ServerAction::enroll;r.profile.id=editing_;r.registration_code=code_.data();r.device_label=label_.data();send(r);code_.fill(0);});
            ImGui::EndDisabled();
            bool registered=false;if(state.servers)for(const auto& p:state.servers->profiles)if(p.id==editing_)registered=p.has_credential;
            ImGui::BeginDisabled(state.busy || !saved || !registered);
            if(ImGui::Button(t("Connect to saved server","저장한 서버에 연결")))safely([&]{protocol::ServerRequest r;r.action=protocol::ServerAction::connect;r.profile.id=editing_;send(r);ImGui::CloseCurrentPopup();});
            ImGui::EndDisabled();
            ImGui::BeginDisabled(state.busy || editing_.empty());
            ImGui::Checkbox(t("Confirm deletion of selected profile","선택한 서버 프로필 삭제 확인"),&remove_);
            if(remove_ && ImGui::Button(t("Delete server","서버 삭제")))safely([&]{protocol::ServerRequest r;r.action=protocol::ServerAction::remove;r.profile.id=editing_;send(r);editing_.clear();remove_=false;ImGui::CloseCurrentPopup();});
            ImGui::EndDisabled();
            if(ImGui::Button(t("Close","닫기"))){credential_.fill(0);code_.fill(0);remove_=false;ImGui::CloseCurrentPopup();}
            if(!state.server_error.empty())ImGui::TextWrapped("%s",core::localized_text(language,state.server_error).data());
            if(!error_.empty())ImGui::TextWrapped("%s",error_.c_str());
            ImGui::EndPopup();
        }
        if(review_){ImGui::OpenPopup("Invitation##server");review_=false;}
        if(ImGui::BeginPopupModal("Invitation##server",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
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
            ImGui::TextUnformatted(t("Network access still requires the network owner's permission.","네트워크 가입에는 해당 네트워크의 권한이 필요합니다."));
            ImGui::BeginDisabled(state.busy || (legacy_ ? (!state.service_online || !state.diagnostics || !state.diagnostics->authenticated) : (!state.servers || (!target_.server.public_key_pin.empty() && !trust_) || !switch_)));
            if(ImGui::Button(t("Accept invitation","초대 수락")))safely([&]{if(legacy_){const auto q=protocol::join_network_command(invitation_,"");model.submit(q.command,q.payload);invitation_.clear();ImGui::CloseCurrentPopup();return;}protocol::ServerRequest r;r.action=protocol::ServerAction::accept_invitation;r.expected_epoch=state.servers->connection_epoch;r.invitation_uri=invitation_;r.approve_server_switch=switch_;r.trust_invitation_pin=trust_;r.device_label=label_[0]?label_.data():"LanLink device";send(r);invitation_.clear();uri_.fill(0);ImGui::CloseCurrentPopup();});
            ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button(t("Cancel","취소"))){invitation_.clear();uri_.fill(0);ImGui::CloseCurrentPopup();}
            ImGui::EndPopup();
        }
        if(!state.server_error.empty())ImGui::TextWrapped("%s",core::localized_text(language,state.server_error).data());
        if(!error_.empty())ImGui::TextWrapped("%s",error_.c_str());
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
    bool await_saved_=false;
    int port_=4433;
    bool legacy_=false,edit_=false,review_=false,trust_=false,switch_=false,remove_=false;
};
}
