#pragma once
#include "lanlink/core/component.hpp"
#include "lanlink/protocol/network_messages.hpp"
#include "lanlink/protocol/game_profiles.hpp"
#include "lanlink/protocol/network_workflow.hpp"
#include "lanlink/protocol/network_overview.hpp"
#include "network_model.hpp"
#include "design.hpp"
#include "management_panel.hpp"
#include "server_panel.hpp"
#include "preferences.hpp"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace lanlink::ui {
namespace {
unsigned hex_digit(const char digit) {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    throw std::invalid_argument("IDs must contain hexadecimal digits");
}

template <std::size_t N>
std::array<std::byte, N> parse_id(const std::string_view text) {
    if (text.size() != N * 2) {
        throw std::invalid_argument("The ID has the wrong length");
    }
    std::array<std::byte, N> id{};
    for (std::size_t i = 0; i < N; ++i) {
        id[i] = static_cast<std::byte>((hex_digit(text[i * 2]) << 4) |
                                        hex_digit(text[i * 2 + 1]));
    }
    return id;
}

template <std::size_t N>
std::string hex_id(const std::array<std::byte, N>& id) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(id.size() * 2);
    for (const auto byte : id) {
        const auto value = std::to_integer<unsigned>(byte);
        text += digits[value >> 4];
        text += digits[value & 15];
    }
    return text;
}

std::string format_bytes(const std::uint64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    double size = static_cast<double>(bytes);
    const char* suffix = "KiB";
    for (const auto* unit : {"KiB", "MiB", "GiB", "TiB"}) {
        size /= 1024.0;
        suffix = unit;
        if (size < 1024.0) break;
    }
    char text[64]{};
    std::snprintf(text, sizeof(text), "%.1f %s", size, suffix);
    return text;
}

}
inline void configure_desktop_style() {
    ImGui::StyleColorsLight();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    auto& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(18, 16);
    style.FramePadding = ImVec2(12, 7);
    style.ItemSpacing = ImVec2(8, 10);
    style.CellPadding = ImVec2(10, 7);
    style.WindowRounding = 10;
    style.ChildRounding = 6;
    style.FrameRounding = 6;
    style.FrameBorderSize = 1;
    style.WindowBorderSize = 1;
    style.PopupBorderSize = 1;
    style.PopupRounding = 10;
    style.ScrollbarSize = 12;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(.96f, .97f, .98f, 1);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(1, 1, 1, 1);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(1, 1, 1, 1);
    style.Colors[ImGuiCol_Text] = ImVec4(.13f, .17f, .23f, 1);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(.40f, .45f, .51f, 1);
    style.Colors[ImGuiCol_Border] = ImVec4(.84f, .87f, .90f, 1);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 1);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(.97f,.98f,1,1);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(.94f,.96f,1,1);
    style.Colors[ImGuiCol_Button] = ImVec4(1, 1, 1, 1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(.94f,.96f,.98f,1);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(.89f,.92f,.96f,1);
    style.Colors[ImGuiCol_Header] = ImVec4(.95f,.97f,.99f,1);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(.80f, .89f, .98f, 1);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(.72f, .84f, .97f, 1);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(1,1,1,1);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(1,1,1,1);
    style.Colors[ImGuiCol_TableHeaderBg] = ImVec4(.96f,.97f,.98f,1);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(1,1,1,1);
    style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(.985f,.989f,.995f,1);
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(.91f,.93f,.95f,1);
    style.Colors[ImGuiCol_Separator] = ImVec4(.86f,.89f,.92f,1);
    style.Colors[ImGuiCol_Tab] = ImVec4(.96f,.97f,.98f,1);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(.89f,.93f,.99f,1);
    style.Colors[ImGuiCol_TabSelected] = ImVec4(.90f,.94f,1,1);
    style.Colors[ImGuiCol_TabSelectedOverline] = design::accent;
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(.10f,.15f,.23f,.25f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(.16f, .38f, .70f, 1);
}
inline void prepare_desktop_popup(float width = 520) {
    const auto available = ImGui::GetMainViewport()->WorkSize;
    width = (std::min)(width, (std::max)(240.0f, available.x - 24));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, (std::max)(200.0f, available.y - 24)));
}
class DesktopView {
public:
    DesktopView(std::optional<core::UiLanguage> language,bool force_management,
        std::string start_label,std::function<void()> start_service)
        :start_label_(std::move(start_label)),start_service_(std::move(start_service)) {
        force_management_=force_management;
        preferences_=load_preferences();
        if(language)preferences_.language=*language;
    }
    void open_invitation(std::string uri){servers_.invitation(std::move(uri));}
    void set_logo(ImTextureID logo){logo_=logo;}
    template <typename Action>
    void attempt(Action&& action) {
        try {
            std::forward<Action>(action)();
            local_error_.clear();
        } catch (const std::exception& error) {
            local_error_ = error.what();
        }
    }

    const char* tr(const char* text) const {
        return lanlink::core::localized_text(preferences_.language,text).data();
    }
    void render() {
        namespace protocol = lanlink::protocol;
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_diagnostics_) {
            attempt([&] {
                networks_.submit(protocol::LocalCommand::server_profiles,protocol::encode_server_request({}));
                networks_.submit(protocol::LocalCommand::diagnostics);
                networks_.submit(protocol::LocalCommand::path_diagnostics);
                networks_.submit(protocol::LocalCommand::game_diagnostics);
                networks_.submit(protocol::LocalCommand::activity);
                if (selected_) networks_.submit(protocol::LocalCommand::peers,
                    protocol::encode_network_selection_request({*selected_}));
                networks_.submit(protocol::LocalCommand::list, protocol::encode_network_list_request());
            });
            next_diagnostics_ = now + std::chrono::seconds{2};
        }
        const auto state = networks_.snapshot();
        if(state.servers && server_epoch_!=state.servers->connection_epoch) {
            if(server_epoch_) {
                selected_.reset();selection_hint_.reset();account_.reset();enrollment_code_.clear();invitation_link_.clear();invitation_code_.clear();
                close_create_=true;close_join_=true;close_confirmation_=true;confirmation_payload_.clear();
                create_password_.fill(0);join_password_.fill(0);network_id_.fill(0);enrollment_input_.fill(0);server_registration_code_.fill(0);
            }
            server_epoch_=state.servers->connection_epoch;
        }
        if(state.operation_generation!=operation_generation_ && state.last_operation) {
            operation_generation_=state.operation_generation;
            const auto& operation=*state.last_operation;
            if(operation.code==protocol::NetworkResultCode::success || operation.code==protocol::NetworkResultCode::pending_approval) {
                if(operation.operation==protocol::NetworkOperation::create)close_create_=true;
                if(operation.operation==protocol::NetworkOperation::join)close_join_=true;
                if(operation.code==protocol::NetworkResultCode::success)selection_hint_=operation.network_id;
            }
        }
        if(selection_hint_ && std::any_of(state.networks.begin(),state.networks.end(),[&](const auto& n){return n.network_id==*selection_hint_;})) {
            selected_=selection_hint_;selection_hint_.reset();
        }
        if(state.access_generation!=access_generation_ && state.access) {
            access_generation_=state.access_generation;
            if(state.access->action<=protocol::AccessAction::device_revoke) {
                account_=state.access;
                if(!state.access->secret.empty())enrollment_code_=state.access->secret;
            }
            if(state.access->action==protocol::AccessAction::invite_issue && state.access->code==protocol::NetworkResultCode::success) {
                invitation_link_=state.access->secret;invitation_code_=state.access->text;invitation_network_=state.access->network_id;
            }
            if(state.access->action==protocol::AccessAction::invite_revoke && state.access->code==protocol::NetworkResultCode::success){invitation_link_.clear();invitation_code_.clear();}
        }
        if (selected_ && std::none_of(state.networks.begin(), state.networks.end(),
                [&](const auto& network) { return network.network_id == *selected_; })) selected_.reset();
        if (!selected_ && !state.networks.empty()) {
            selected_ = state.networks.front().network_id;
            networks_.submit(protocol::LocalCommand::peers, protocol::encode_network_selection_request({*selected_}));
        }
        if (state.diagnostics &&
            state.diagnostics_generation != last_diagnostics_generation_) {
            const auto& current = *state.diagnostics;
            if (last_diagnostics_generation_ != 0 && last_connected_ &&
                current.connected && last_tcp_ == current.tcp_fallback &&
                last_reconnects_ == current.reconnects &&
                current.sent_bytes >= last_sent_bytes_ &&
                current.received_bytes >= last_received_bytes_) {
                const auto elapsed = std::chrono::duration<double>(
                    now - last_diagnostics_time_).count();
                if (elapsed > 0) {
                    send_rate_ = (current.sent_bytes - last_sent_bytes_) / elapsed;
                    receive_rate_ = (current.received_bytes - last_received_bytes_) / elapsed;
                }
            } else {
                send_rate_ = 0;
                receive_rate_ = 0;
            }
            last_diagnostics_generation_ = state.diagnostics_generation;
            last_diagnostics_time_ = now;
            last_sent_bytes_ = current.sent_bytes;
            last_received_bytes_ = current.received_bytes;
            last_tcp_ = current.tcp_fallback;
            last_connected_ = current.connected;
            last_reconnects_ = current.reconnects;
        }
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0);
        ImGui::Begin("LanLink", nullptr, flags);
        ImGui::PopStyleVar(2);
        const bool connected = state.service_online && state.diagnostics && state.diagnostics->authenticated;
        const bool unconfigured = state.service_online && state.diagnostics &&
            state.diagnostics->configuration == protocol::ClientConfigurationState::unconfigured;
        const bool connection_error = state.diagnostics &&
            (state.diagnostics->configuration == protocol::ClientConfigurationState::invalid_credentials ||
             !state.diagnostics->last_error.empty());
        ImGui::Image(logo_, ImVec2(30,30)); ImGui::SameLine(0,10);
        design::title("LanLink");
        const char* connection_label=connected?tr("Connected"):connection_error?tr("Connection error"):
            !state.service_online?tr("Service unavailable"):state.servers && !state.servers->selected_id.empty()?tr("Connecting"):tr("Disconnected");
        const float badge_width=ImGui::CalcTextSize(connection_label).x+30;
        ImGui::SameLine((std::max)(210.0f,ImGui::GetWindowContentRegionMax().x-badge_width-46));
        design::status(connection_label,connected,connection_error);
        ImGui::SameLine(0,12);
        if(design::icon_button("##settings",design::Icon::settings,tr("Settings")))settings_open_=true;
        ImGui::Dummy(ImVec2(0,4));
        const protocol::ServerProfile* active = nullptr;
        if(state.servers) {
            if(!state.servers->selected_id.empty())preferred_server_=state.servers->selected_id;
            for(const auto& p:state.servers->profiles)if(p.id==preferred_server_)active=&p;
            if(!active && !state.servers->profiles.empty())active=&state.servers->profiles.front();
        }
        const bool connection_requested=state.servers && !state.servers->selected_id.empty();
        const float card_width=ImGui::GetContentRegionAvail().x;
        const auto card=ImGui::GetCursorScreenPos();
        auto* draw=ImGui::GetWindowDrawList();
        draw->AddRectFilled(ImVec2(card.x,card.y+2),ImVec2(card.x+card_width,card.y+80),IM_COL32(25,40,65,8),10);
        draw->AddRectFilled(card,ImVec2(card.x+card_width,card.y+78),IM_COL32(255,255,255,255),10);
        draw->AddRect(card,ImVec2(card.x+card_width,card.y+78),ImGui::GetColorU32(design::border),10);
        ImGui::InvisibleButton("##server-picker",ImVec2(card_width-94,78),ImGuiButtonFlags_EnableNav);
        const bool pick_server=ImGui::IsItemActivated();
        const bool picker_hover=ImGui::IsItemHovered();
        if(ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)draw->AddRect(card,ImVec2(card.x+card_width-94,card.y+78),ImGui::GetColorU32(design::accent),10);
        draw->AddText(ImVec2(card.x+16,card.y+11),ImGui::GetColorU32(design::muted),tr("Server"));
        auto* face=design::emphasis_font?design::emphasis_font:ImGui::GetFont();
        const ImVec4 clip(card.x+16,card.y+31,card.x+card_width-132,card.y+68);
        draw->AddText(face,16,ImVec2(card.x+16,card.y+36),ImGui::GetColorU32(design::ink),active?active->name.c_str():tr("Select server"),nullptr,0,&clip);
        design::icon(design::Icon::chevron,ImVec2(card.x+card_width-125,card.y+31),20,ImGui::GetColorU32(design::muted));
        if(active && picker_hover)ImGui::SetTooltip("%s:%u",active->host.c_str(),active->port);
        if(pick_server)ImGui::OpenPopup("server-picker");
        ImGui::SetNextWindowSizeConstraints(ImVec2((std::min)(320.0f,card_width-94),0),ImVec2(card_width-94,260));
        if(ImGui::BeginPopup("server-picker")) {
            if(state.servers)for(const auto& p:state.servers->profiles) {
                ImGui::PushID(p.id.c_str());
                ImGui::BeginDisabled(state.busy);
                if(ImGui::Selectable(p.name.c_str(),active==&p)) {
                    preferred_server_=p.id;
                    if(!p.has_credential)servers_.edit_server(p);
                    else attempt([&]{protocol::ServerRequest request;request.action=protocol::ServerAction::connect;request.profile.id=p.id;
                        networks_.submit(protocol::LocalCommand::server_profiles,protocol::encode_server_request(request));});
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::Separator();
            if(ImGui::Selectable(tr("Add server")))servers_.add_server();
            if(ImGui::Selectable(tr("Use invitation")))servers_.paste_invitation();
            ImGui::EndPopup();
        }
        ImGui::SetCursorScreenPos(ImVec2(card.x+card_width-86,card.y+22));
        ImGui::BeginDisabled(state.busy || !active);
        if(design::icon_button("##disconnect",design::Icon::power,tr(connection_requested?"Disconnect":"Connect"),connected))attempt([&]{
            if(!connection_requested && active && !active->has_credential){servers_.edit_server(*active);return;}
            protocol::ServerRequest request;request.action=connection_requested?protocol::ServerAction::disconnect:protocol::ServerAction::connect;
            if(active)request.profile.id=active->id;
            networks_.submit(protocol::LocalCommand::server_profiles,protocol::encode_server_request(request));
        });
        ImGui::EndDisabled();ImGui::SameLine(0,4);
        if(design::icon_button("##servers",design::Icon::more,tr("Servers")))servers_.open_manager();
        ImGui::SetCursorScreenPos(ImVec2(card.x,card.y+92));
        if (!selected_ && !state.networks.empty()) {
            selected_ = state.networks.front().network_id;
            attempt([&] { networks_.submit(protocol::LocalCommand::peers,
                protocol::encode_network_selection_request({*selected_})); });
        }
        const auto selected = std::find_if(state.networks.begin(), state.networks.end(),
            [&](const auto& network) { return selected_ && network.network_id == *selected_; });
        ImGui::AlignTextToFramePadding();
        design::emphasis(tr("Your networks"));
        if(!state.networks.empty()) {
            ImGui::SameLine((std::max)(140.0f,ImGui::GetWindowContentRegionMax().x-170));
            ImGui::BeginDisabled(!connected || state.busy);
            if(design::button(tr("New network"),ImVec2(82,30)))create_dialog_=true;
            ImGui::SameLine();if(design::button(tr("Join"),ImVec2(80,30)))join_dialog_=true;
            ImGui::EndDisabled();
            ImGui::SetNextItemWidth(-1);
            if(ImGui::BeginCombo("##networks",selected!=state.networks.end()?selected->name.c_str():tr("Your networks"))) {
                for(const auto& network:state.networks)if(ImGui::Selectable(network.name.c_str(),selected_==network.network_id)) {
                    selected_=network.network_id;
                    attempt([&]{networks_.submit(protocol::LocalCommand::peers,protocol::encode_network_selection_request({network.network_id}));});
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::BeginChild("workspace", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_None)) {
            if (selected == state.networks.end()) {
                const float center=ImGui::GetContentRegionAvail().x*.5f;
                ImGui::Dummy(ImVec2(0,(std::max)(10.0f,ImGui::GetContentRegionAvail().y*.17f)));
                const auto glyph=ImGui::GetCursorScreenPos();
                const auto center_point=ImVec2(glyph.x+center,glyph.y+28);
                ImGui::GetWindowDrawList()->AddCircleFilled(center_point,29,IM_COL32(237,243,252,255));
                design::icon(design::Icon::network,ImVec2(center_point.x-16,center_point.y-16),32,ImGui::GetColorU32(design::accent));
                ImGui::Dummy(ImVec2(0,66));
                const char* heading=!state.service_online?tr("Service unavailable"):!connected?tr("No server connected"):tr("No networks yet");
                auto* heading_face=design::emphasis_font?design::emphasis_font:ImGui::GetFont();
                const float heading_width=heading_face->CalcTextSizeA(16,1000,0,heading).x;
                ImGui::SetCursorPosX((std::max)(0.0f,center-heading_width*.5f));design::emphasis(heading);
                const char* hint=!state.service_online?tr("Try starting LanLink again."):!connected?tr("Add a server or use an invitation."):tr("Create a network or join your friends.");
                ImGui::SetCursorPosX((std::max)(0.0f,center-ImGui::CalcTextSize(hint).x*.5f));
                ImGui::TextDisabled("%s",hint);ImGui::Dummy(ImVec2(0,7));
                ImGui::SetCursorPosX((std::max)(0.0f,center-144));
                if(!state.service_online) {
                    if(design::button(tr("Retry"),ImVec2(288,36),true))attempt([&]{start_service_();next_diagnostics_={};});
                } else if(!connected) {
                    if(design::button(tr("Add server"),ImVec2(140,36),true))servers_.add_server();
                    ImGui::SameLine();if(design::button(tr("Use invitation"),ImVec2(140,36)))servers_.paste_invitation();
                } else {
                    ImGui::BeginDisabled(state.busy);
                    if(design::button(tr("Create network"),ImVec2(140,36),true))create_dialog_=true;
                    ImGui::SameLine();if(design::button(tr("Join network"),ImVec2(140,36)))join_dialog_=true;
                    ImGui::EndDisabled();
                }

            } else {
                const auto id = hex_id(selected->network_id);
                const bool owner = selected->role == protocol::NetworkRole::owner;
                const bool administrator = owner || (state.peers && state.peers->network_id == selected->network_id &&
                    state.peers->policy && protocol::policy_role(*state.peers->policy, state.activity.device_id) == protocol::AccessRole::administrator);
                if (state.peers && state.peers->network_id == selected->network_id &&
                    now-state.peers_updated_at<std::chrono::seconds(6)) {
                    const auto& peers = *state.peers;
                    const auto own = ipv4(peers.own_address);
                    const auto* current_paths=state.paths && now-state.paths_updated_at<std::chrono::seconds(6) ? &*state.paths : nullptr;
                    const auto overview=protocol::network_overview(peers,current_paths,connected,state.activity.device_id);
                    ImGui::AlignTextToFramePadding();ImGui::TextDisabled("%s",tr("My virtual IP"));ImGui::SameLine(0,12);
                    design::emphasis(own.c_str());ImGui::SameLine(0,8);
                    if(design::icon_button("##copy-ip",design::Icon::copy,tr("Copy IP")))ImGui::SetClipboardText(own.c_str());
                    ImGui::Spacing();
                    design::emphasis(tr("Devices"));ImGui::SameLine();
                    ImGui::TextDisabled(tr("%u / %u online"),overview.online,overview.total);
                    ImGui::Spacing();

                    if (ImGui::BeginTable("devices", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn(tr("Virtual IP"), ImGuiTableColumnFlags_WidthFixed, 112);
                        ImGui::TableSetupColumn(tr("Device"));
                        ImGui::TableSetupColumn(tr("Status"), ImGuiTableColumnFlags_WidthFixed, 66);
                        ImGui::TableSetupColumn(tr("Path"), ImGuiTableColumnFlags_WidthFixed, 92);
                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 66);
                        ImGui::TableHeadersRow();
                        const auto row = [&](const protocol::NetworkDeviceId& device_id, std::uint32_t address, bool online, bool own_device) {
                            const auto device_text = hex_id(device_id);
                            const auto ip = ipv4(address);
                            ImGui::PushID(device_text.c_str());
                            ImGui::TableNextRow(); ImGui::TableNextColumn();
                            if (ImGui::Selectable(ip.c_str())) ImGui::SetClipboardText(ip.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("%s%s", own_device ? tr("This device") : device_text.substr(0,12).c_str(), own_device ? "" : "...");
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", device_text.c_str());
                            ImGui::TableNextColumn();
                            const auto status_position=ImGui::GetCursorScreenPos();
                            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(status_position.x+4,status_position.y+8),3,ImGui::GetColorU32(online?design::green:design::muted));
                            ImGui::SetCursorPosX(ImGui::GetCursorPosX()+13);
                            ImGui::TextColored(online?design::green:design::muted,"%s",online?tr("Online"):tr("Offline"));
                            ImGui::TableNextColumn();
                            const protocol::PeerPathStatus* path=nullptr;
                            if(current_paths)for(const auto& peer:current_paths->peers)
                                if(peer.network_id==selected->network_id && peer.device_id==device_id){path=&peer;break;}
                            const auto kind=path && path->path.lifecycle==protocol::PathLifecycle::authenticated ? path->path.kind : protocol::PathKind::none;
                            const bool blocked=!own_device && peers.policy && !protocol::policy_allows(*peers.policy,state.activity.device_id,device_id);
                            const char* label=blocked?tr("Blocked"):own_device?tr("Local"):!online?tr("Offline"):kind==protocol::PathKind::direct_quic?tr("Direct"):kind==protocol::PathKind::relay_tls?tr("Relay (TCP)"):kind==protocol::PathKind::relay_quic?tr("Relay"):tr("Connecting");
                            ImGui::TextColored(kind==protocol::PathKind::direct_quic?ImVec4(.10f,.48f,.32f,1):ImVec4(.25f,.35f,.48f,1),"%s",label);
                            if(blocked && ImGui::IsItemHovered())ImGui::SetTooltip("%s",tr("Blocked by policy"));
                            if(!blocked && path && ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();ImGui::TextUnformatted(tr(protocol::path_reason_name(path->path.reason).data()));
                                if(path->path.rtt_us)ImGui::Text("RTT: %.1f ms",*path->path.rtt_us/1000.0);
                                ImGui::Text(tr("Packets: %llu sent / %llu received"),static_cast<unsigned long long>(path->path.sent_packets),static_cast<unsigned long long>(path->path.received_packets));
                                if(path->path.lost_packets)ImGui::Text(tr("Lost transport packets: %llu"),static_cast<unsigned long long>(*path->path.lost_packets));
                                ImGui::EndTooltip();
                            }
                            ImGui::TableNextColumn();
                            ImGui::BeginDisabled(!administrator || own_device || device_id==selected->owner_device_id || (!owner && peers.policy && protocol::policy_role(*peers.policy,device_id)==protocol::AccessRole::administrator) || !connected || state.busy);
                            if (ImGui::SmallButton(tr("Remove"))) {
                                confirmation_ = protocol::LocalCommand::kick;
                                confirmation_payload_ = protocol::encode_network_member_request({selected->network_id, device_id});
                                confirmation_text_ = tr("Remove this device from the network? Its network access will be revoked.");
                                confirm_dialog_ = true;
                            }
                            ImGui::EndDisabled(); ImGui::PopID();
                        };
                        row(state.activity.device_id, peers.own_address, connected, true);
                        for (const auto& peer : peers.peers) row(peer.device_id, peer.ipv4_address, connected && peer.signed_key.has_value(), false);
                        ImGui::EndTable();
                    }
                    if (administrator && ImGui::CollapsingHeader(tr("Join requests"))) {
                        bool any_requests = false;
                        for (const auto& request : state.activity.requests) {
                            if (request.network_id != selected->network_id) continue;
                            any_requests = true;
                            const auto device_text = hex_id(request.device_id);
                            ImGui::PushID(device_text.c_str());
                            ImGui::Text("%s...", device_text.substr(0, 24).c_str());
                            ImGui::SameLine();
                            ImGui::BeginDisabled(!connected || state.busy);
                            if (ImGui::SmallButton(tr("Approve"))) attempt([&] {
                                networks_.submit(protocol::LocalCommand::approve, protocol::encode_network_member_request({selected->network_id, request.device_id}));
                            });
                            ImGui::EndDisabled(); ImGui::PopID();
                        }
                        if (!any_requests) ImGui::TextDisabled("%s", tr("No new requests in this service session."));
                        if (ImGui::CollapsingHeader(tr("Manage a device by ID"))) {
                            ImGui::SetNextItemWidth(-1);
                            ImGui::InputTextWithHint("##device", tr("Device ID"), device_id_.data(), device_id_.size());
                            ImGui::BeginDisabled(!connected || state.busy || device_id_[0] == 0);
                            for (const auto& action : {std::pair{tr("Invite"), protocol::LocalCommand::invite}, std::pair{tr("Approve"), protocol::LocalCommand::approve}}) {
                                if (ImGui::Button(action.first)) attempt([&] {
                                    networks_.submit(action.second, protocol::encode_network_member_request({selected->network_id,
                                        parse_id<protocol::network_device_id_size>(device_id_.data())}));
                                });
                                ImGui::SameLine();
                            }
                            ImGui::NewLine(); ImGui::EndDisabled();
                        }
                    }
                } else ImGui::TextWrapped("%s", tr("Waiting for an active network address. The owner may still need to approve this device."));

                if (ImGui::CollapsingHeader(tr("Network options"))) {
                if (ImGui::Button(tr("Copy network ID"))) ImGui::SetClipboardText(id.c_str());
                ImGui::SameLine();
                ImGui::BeginDisabled(!connected || state.busy);
                if(administrator && ImGui::Button(tr("Create invitation"))) attempt([&]{
                    protocol::AccessRequest r;r.action=protocol::AccessAction::invite_issue;r.network_id=selected->network_id;
                    r.lifetime_seconds=900;r.max_uses=1;
                    networks_.submit(protocol::LocalCommand::user_access,protocol::encode_access_request(r));
                });
                if(administrator)ImGui::SameLine();
                if (ImGui::Button(tr("Leave network"))) {
                    confirmation_ = protocol::LocalCommand::leave;
                    confirmation_payload_ = protocol::encode_network_selection_request({selected->network_id});
                    confirmation_text_ = tr("Leave this network? You will need approval to join it again.");
                    confirm_dialog_ = true;
                }
                ImGui::EndDisabled();
                if(administrator && invitation_network_==selected->network_id && !invitation_link_.empty()) {
                    ImGui::TextWrapped("%s", tr("Invitation: one use, expires in 15 minutes. Share it privately."));
                    if(ImGui::Button(tr("Copy invitation link")))ImGui::SetClipboardText(invitation_link_.c_str());
                    if(state.servers) {
                        ImGui::InputText(tr("Server registration code (optional)"),server_registration_code_.data(),server_registration_code_.size(),ImGuiInputTextFlags_Password);
                        if(ImGui::Button(tr("Copy server and network invitation")))attempt([&]{
                            const auto& s=*state.servers;auto p=std::find_if(s.profiles.begin(),s.profiles.end(),[&](const auto& p){return p.id==s.selected_id;});
                            if(p==s.profiles.end())throw std::runtime_error("Connect to a server first");
                            protocol::ServerInvitation invite;invite.server=*p;invite.server.id.clear();invite.server.has_credential=false;invite.registration_code=server_registration_code_.data();invite.network_id=invitation_network_;
                            invite.network_invitation=protocol::parse_join_target(invitation_link_).invitation;
                            ImGui::SetClipboardText(protocol::make_server_invitation(invite).c_str());server_registration_code_.fill(0);
                        });
                    }
                    ImGui::SameLine();if(ImGui::Button(tr("Copy invitation code")))ImGui::SetClipboardText(invitation_code_.c_str());
                }
                ImGui::BeginDisabled(!connected || state.busy);
                if(administrator && ImGui::SmallButton(tr("Revoke invitation links"))) {
                    protocol::AccessRequest r;r.action=protocol::AccessAction::invite_revoke;r.network_id=selected->network_id;
                    confirmation_=protocol::LocalCommand::user_access;confirmation_payload_=protocol::encode_access_request(r);
                    confirmation_text_=tr("Revoke all unused invitation links for this network?");confirm_dialog_=true;
                }
                ImGui::EndDisabled();
                }
                management_.draw(networks_, state, *selected, preferences_.language, force_management_);
            }
        }
        ImGui::EndChild();
        const char* notice=state.busy?tr("Updating..."):!local_error_.empty()?tr(local_error_.c_str()):
            !state.server_error.empty()?tr(state.server_error.c_str()):!state.error.empty()?tr("Connection error"):
            !state.message.empty()?tr(state.message.c_str()):"";
        if(*notice) {
            const auto p=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x;
            const ImVec4 clip(p.x,p.y,p.x+width,p.y+ImGui::GetTextLineHeight());
            ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),ImGui::GetFontSize(),p,ImGui::GetColorU32(design::muted),notice,nullptr,0,&clip);
            ImGui::Dummy(ImVec2(width,ImGui::GetTextLineHeight()));
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",notice);
        }
        servers_.draw(networks_, state, preferences_.language);
        if (settings_open_) { ImGui::OpenPopup(tr("Settings")); settings_open_ = false; }
        prepare_desktop_popup();
        bool settings_visible = true;
        if (ImGui::BeginPopupModal(tr("Settings"), &settings_visible, ImGuiWindowFlags_AlwaysAutoResize)) {
            int language = preferences_.language == lanlink::core::UiLanguage::korean ? 1 : 0;
            const char* languages[]{"English", "한국어"};
            ImGui::TextUnformatted(tr("Language")); ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##language", &language, languages, 2)) {
                preferences_.language = language == 1 ? core::UiLanguage::korean : core::UiLanguage::english;
                attempt([&] { save_preferences(preferences_); });
                ImGui::CloseCurrentPopup(); settings_open_ = true;
            }
            if(design::button(tr("Diagnostics"))){diagnostics_open_=true;ImGui::CloseCurrentPopup();}
            ImGui::SeparatorText(tr("This device"));
            const auto device = hex_id(state.activity.device_id);
            const bool has_identity = std::any_of(state.activity.device_id.begin(), state.activity.device_id.end(),
                [](std::byte value) { return value != std::byte{0}; });
            ImGui::BeginDisabled(!has_identity);
            if (ImGui::Button(tr("Copy device ID"))) ImGui::SetClipboardText(device.c_str());
            ImGui::EndDisabled();
            if (!state.service_online && ImGui::Button(tr(start_label_.c_str())))
                attempt([&] { start_service_(); next_diagnostics_ = {}; });
            if(ImGui::CollapsingHeader(tr("Account (optional)"))) {
                ImGui::TextWrapped("%s", tr("Group your devices. Server registration works without an account."));
                ImGui::BeginDisabled(!connected || state.busy);
                if(ImGui::Button(tr("Refresh account")))attempt([&]{protocol::AccessRequest r;r.action=protocol::AccessAction::account_status;networks_.submit(protocol::LocalCommand::user_access,protocol::encode_access_request(r));});
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##accountname",tr("Account or device name"),account_name_.data(),account_name_.size());
                if(ImGui::Button(tr("Create optional account")))attempt([&]{protocol::AccessRequest r;r.action=protocol::AccessAction::account_create;r.text=account_name_.data();networks_.submit(protocol::LocalCommand::user_access,protocol::encode_access_request(r));});
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##enrollment",tr("Enrollment code"),enrollment_input_.data(),enrollment_input_.size(),ImGuiInputTextFlags_Password);
                if(ImGui::Button(tr("Enroll this device")))attempt([&]{protocol::AccessRequest r;r.action=protocol::AccessAction::device_enroll;r.text=account_name_.data();r.secret=enrollment_input_.data();networks_.submit(protocol::LocalCommand::user_access,protocol::encode_access_request(r));std::fill(enrollment_input_.begin(),enrollment_input_.end(),0);});
                if(account_ && account_->code==protocol::NetworkResultCode::success) {
                    ImGui::TextWrapped("%s",account_->text.c_str());
                    if(ImGui::Button(tr("Generate enrollment code")))attempt([&]{protocol::AccessRequest r;r.action=protocol::AccessAction::enrollment_issue;networks_.submit(protocol::LocalCommand::user_access,protocol::encode_access_request(r));});
                    if(!enrollment_code_.empty() && ImGui::Button(tr("Copy enrollment code")))ImGui::SetClipboardText(enrollment_code_.c_str());
                    if(!enrollment_code_.empty())ImGui::TextWrapped("%s", tr("Enrollment expires after five minutes and one use."));
                    for(const auto& enrolled:account_->devices) {
                        const auto enrolled_id=hex_id(enrolled.device_id);ImGui::PushID(enrolled_id.c_str());
                        ImGui::TextWrapped("%s%s",enrolled.label.c_str(),enrolled.revoked ? tr(" (revoked)") : "");
                        if(!enrolled.revoked && enrolled.device_id!=state.activity.device_id && ImGui::SmallButton(tr("Revoke device"))) {
                            protocol::AccessRequest r;r.action=protocol::AccessAction::device_revoke;r.device_id=enrolled.device_id;
                            confirmation_=protocol::LocalCommand::user_access;confirmation_payload_=protocol::encode_access_request(r);
                            confirmation_text_=tr("Revoke this enrolled device and remove its network access?");confirm_dialog_=true;
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndDisabled();
            }
            ImGui::Separator();
            ImGui::TextDisabled("LanLink %s", core::project_version().data());
            if (ImGui::Button(tr("Close"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (diagnostics_open_) { ImGui::OpenPopup(tr("Diagnostics")); diagnostics_open_ = false; }
        prepare_desktop_popup(600);
        bool diagnostics_visible = true;
        if (ImGui::BeginPopupModal(tr("Diagnostics"), &diagnostics_visible, ImGuiWindowFlags_AlwaysAutoResize)) {
            if(!state.error.empty())ImGui::TextWrapped("%s",state.error.c_str());
            if(!state.server_error.empty())ImGui::TextWrapped("%s",tr(state.server_error.c_str()));
            if(state.servers && !state.servers->notice.empty())ImGui::TextWrapped("%s",tr(state.servers->notice.c_str()));
            if (ImGui::CollapsingHeader(tr("Game network diagnostics"), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (state.games) {
                    const auto& metrics = *state.games;
                    ImGui::Text(tr("Virtual adapter MTU: %u  /  Rejected packets: %llu"),
                        static_cast<unsigned>(metrics.mtu), static_cast<unsigned long long>(metrics.rejected_packets));
                    if (metrics.adapter_failed)
                        ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "%s", tr("The virtual adapter failed. Restart the LanLink service."));
                    const auto current = std::find_if(metrics.networks.begin(), metrics.networks.end(),
                        [&](const auto& item) { return selected_ && item.network_id == *selected_; });
                    if (current != metrics.networks.end()) {
                        ImGui::Text(tr("Virtual subnet: %s/%u  /  Online peers: %u"),
                            ipv4(current->subnet).c_str(), static_cast<unsigned>(current->prefix_length), current->online_peers);
                        ImGui::Text(tr("Discovery: %llu sent / %llu received"),
                            static_cast<unsigned long long>(current->discovery_sent),
                            static_cast<unsigned long long>(current->discovery_received));
                        ImGui::TextDisabled(tr("Suppressed: %llu rate limited / %llu duplicates / %llu reflections"),
                            static_cast<unsigned long long>(current->rate_limited),
                            static_cast<unsigned long long>(current->duplicates),
                            static_cast<unsigned long long>(current->reflections));
                        if (!current->online_peers)
                            ImGui::TextWrapped("%s", tr("No other authenticated device is online. Check that the host joined this network and was approved."));
                        else if (!current->discovery_received)
                            ImGui::TextWrapped("%s", tr("No inbound discovery has been observed in this session. Refresh the game's LAN list and check the host's game server and firewall rules."));
                        if (ImGui::Button(tr("Copy game diagnostics"))) {
                            const auto report = std::string(tr("LanLink game diagnostics\nVirtual IP: ")) + ipv4(current->address) +
                                tr("\nSubnet: ") + ipv4(current->subnet) + "/" + std::to_string(current->prefix_length) +
                                "\nMTU: " + std::to_string(metrics.mtu) +
                                tr("\nOnline peers: ") + std::to_string(current->online_peers) +
                                tr("\nDiscovery sent/received: ") + std::to_string(current->discovery_sent) + "/" +
                                std::to_string(current->discovery_received) +
                                tr("\nRate limited: ") + std::to_string(current->rate_limited) +
                                tr("\nFirewall/game server status: not measured");
                            ImGui::SetClipboardText(report.c_str());
                        }
                    } else ImGui::TextWrapped("%s", tr("Select an approved network with an active virtual address."));
                    if (metrics.truncated) ImGui::TextDisabled("%s", tr("Showing a limited number of network diagnostics."));
                } else ImGui::TextWrapped("%s", tr("Game telemetry is unavailable. Check the background service and use matching UI/service versions."));
                ImGui::TextWrapped("%s", tr("If LAN search fails, connect to the host's virtual IPv4 and game port. Both devices must use the same game version and virtual network."));
                ImGui::TextWrapped("%s", tr("Allow the game's required inbound ports on the LanLink adapter. Firewall and game-server rules are not measured automatically."));
                const auto profiles = protocol::game_profiles();
                const char* selection = game_profile_ >= 0 && static_cast<std::size_t>(game_profile_) < profiles.size()
                    ? profiles[static_cast<std::size_t>(game_profile_)].name.data() : tr("General guidance");
                if (ImGui::BeginCombo(tr("Game profile"), selection)) {
                    if (ImGui::Selectable(tr("General guidance"), game_profile_ == -1)) game_profile_ = -1;
                    for (std::size_t index = 0; index < profiles.size(); ++index)
                        if (ImGui::Selectable(profiles[index].name.data(), game_profile_ == static_cast<int>(index)))
                            game_profile_ = static_cast<int>(index);
                    ImGui::EndCombo();
                }
                if (game_profile_ >= 0 && static_cast<std::size_t>(game_profile_) < profiles.size()) {
                    const auto& profile = profiles[static_cast<std::size_t>(game_profile_)];
                    ImGui::Text(tr("Version %s / UDP %u-%u"), profile.version.data(),
                        static_cast<unsigned>(profile.first_port), static_cast<unsigned>(profile.last_port));
                    ImGui::TextWrapped("%s", tr(profile.guidance.data()));
                }
            }
            if (ImGui::CollapsingHeader(tr("Connection details"), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (state.diagnostics) {
                    const auto& d = *state.diagnostics;
                    if (unconfigured) ImGui::TextUnformatted(tr("No server configured"));
                    else ImGui::Text(tr("Relay transport: %s   /   Reconnects: %u"), d.tcp_fallback ? tr("TLS over TCP") : tr("QUIC over UDP"), d.reconnects);
                    if (d.rtt_us) ImGui::Text(tr("Relay latency: %.1f ms"), *d.rtt_us / 1000.0);
                    ImGui::Text(tr("Upload %.1f KiB/s   /   Download %.1f KiB/s"), send_rate_ / 1024.0, receive_rate_ / 1024.0);
                    ImGui::TextDisabled(tr("Sent %s   /   Received %s"), format_bytes(d.sent_bytes).c_str(), format_bytes(d.received_bytes).c_str());
                    if(state.paths) {
                        unsigned direct=0,relayed=0;for(const auto& peer:state.paths->peers) {
                            direct+=peer.path.kind==protocol::PathKind::direct_quic;
                            relayed+=peer.path.kind==protocol::PathKind::relay_quic || peer.path.kind==protocol::PathKind::relay_tls;
                        }
                        ImGui::Text(tr("Peer paths: %u Direct / %u Relay"),direct,relayed);
                        if(state.paths->truncated)ImGui::TextDisabled("%s", tr("Showing a limited number of peer paths."));
                    }
                    if (!d.last_error.empty()) ImGui::TextWrapped(tr("Last connection error: %s"), d.last_error.c_str());
                } else ImGui::TextWrapped("%s", tr("The background service is unavailable. Start the service or repair the LanLink installation."));
            }
            if (ImGui::Button(tr("Close"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (create_dialog_) { close_create_=false;ImGui::OpenPopup(tr("Create network")); create_dialog_ = false; }
        if (join_dialog_) { close_join_=false;ImGui::OpenPopup(tr("Join network")); join_dialog_ = false; }
        if (confirm_dialog_) { ImGui::OpenPopup(tr("Confirm network change")); confirm_dialog_ = false; }
        prepare_desktop_popup(460);
        if (ImGui::BeginPopupModal(tr("Create network"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(tr("Name your private network."));
            ImGui::SetNextItemWidth(380);
            ImGui::InputTextWithHint("##name", tr("Network name"), name_.data(), name_.size());
            ImGui::SetNextItemWidth(380);
            ImGui::InputTextWithHint("##createpassword",tr("Password (optional)"),create_password_.data(),create_password_.size(),ImGuiInputTextFlags_Password);
            if(create_password_[0])ImGui::Checkbox(tr("Require owner approval"),&require_approval_);
            ImGui::BeginDisabled(!connected || state.busy || name_[0] == 0);
            if (ImGui::Button(tr("Create"), ImVec2(180, 0))) {
                attempt([&] { const auto request=protocol::create_network_command(name_.data(),create_password_.data(),require_approval_);
                    networks_.submit(request.command,request.payload); });
                if(local_error_.empty())std::fill(create_password_.begin(),create_password_.end(),0);
            }
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button(tr("Cancel"), ImVec2(180, 0))) {std::fill(create_password_.begin(),create_password_.end(),0);ImGui::CloseCurrentPopup();}
            if(!state.error.empty())ImGui::TextWrapped("%s",tr(state.error.c_str()));
            if(!local_error_.empty())ImGui::TextWrapped("%s",tr(local_error_.c_str()));
            if(close_create_){ImGui::CloseCurrentPopup();close_create_=false;}
            ImGui::EndPopup();
        }
        prepare_desktop_popup(480);
        if (ImGui::BeginPopupModal(tr("Join network"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(tr("Paste a network ID, invitation link or invitation code."));
            ImGui::SetNextItemWidth(420);
            ImGui::InputTextWithHint("##network", tr("Network ID or invitation"), network_id_.data(), network_id_.size());
            ImGui::SetNextItemWidth(420);
            ImGui::InputTextWithHint("##joinpassword",tr("Network password (if required)"),join_password_.data(),join_password_.size(),ImGuiInputTextFlags_Password);
            ImGui::BeginDisabled(!connected || state.busy || network_id_[0] == 0);
            if (ImGui::Button(tr("Send join request"), ImVec2(200, 0))) {
                attempt([&] {if(std::string_view(network_id_.data()).starts_with("lanlink://connect/")){open_invitation(network_id_.data());return;}const auto request=protocol::join_network_command(network_id_.data(),join_password_.data());
                    networks_.submit(request.command,request.payload);});
                if(local_error_.empty())std::fill(join_password_.begin(),join_password_.end(),0);
            }
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button(tr("Cancel"), ImVec2(200, 0))) {std::fill(join_password_.begin(),join_password_.end(),0);ImGui::CloseCurrentPopup();}
            if(close_join_){ImGui::CloseCurrentPopup();close_join_=false;}
            if(!state.error.empty())ImGui::TextWrapped("%s",tr(state.error.c_str()));
            if (!local_error_.empty()) ImGui::TextWrapped("%s", tr(local_error_.c_str()));
            ImGui::EndPopup();
        }
        prepare_desktop_popup(460);
        if (ImGui::BeginPopupModal(tr("Confirm network change"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if(close_confirmation_){ImGui::CloseCurrentPopup();close_confirmation_=false;}
            ImGui::TextWrapped("%s", tr(confirmation_text_.c_str()));
            ImGui::BeginDisabled(!connected || state.busy);
            if (ImGui::Button(tr("Confirm"), ImVec2(170, 0))) {
                attempt([&] { networks_.submit(confirmation_, confirmation_payload_); });
                if (local_error_.empty()) ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::Button(tr("Cancel"), ImVec2(170, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::End();
    }

    static std::string ipv4(std::uint32_t address) {
        return std::to_string(address >> 24) + "." + std::to_string((address >> 16) & 255) + "." +
            std::to_string((address >> 8) & 255) + "." + std::to_string(address & 255);
    }

private:
    ServerPanel servers_;
    std::string preferred_server_;
    std::uint64_t server_epoch_=0;
    bool close_confirmation_=false;
    std::array<char,41> server_registration_code_{};
    ImTextureID logo_=0;
    std::string start_label_;
    std::function<void()> start_service_;
    lanlink::ui::NetworkModel networks_;
    lanlink::ui::ManagementPanel management_;
    bool force_management_=false;
    lanlink::ui::Preferences preferences_;
    bool settings_open_=false, diagnostics_open_=false;
    std::optional<lanlink::protocol::NetworkId> selected_;
    std::array<char, 129> name_{};
    std::array<char, 2049> network_id_{};
    std::array<char,129> create_password_{},join_password_{};
    bool require_approval_=true,close_create_=false,close_join_=false;
    std::optional<lanlink::protocol::NetworkId> selection_hint_;
    std::uint64_t operation_generation_=0,access_generation_=0;
    lanlink::protocol::NetworkId invitation_network_{};
    std::string invitation_link_,invitation_code_;
    std::optional<lanlink::protocol::AccessResult> account_;
    std::array<char,129> account_name_{};
    std::array<char,41> enrollment_input_{};
    std::string enrollment_code_;
    std::array<char, 65> device_id_{};
    std::string local_error_;
    bool create_dialog_ = false;
    bool join_dialog_ = false;
    bool confirm_dialog_ = false;
    lanlink::protocol::LocalCommand confirmation_ = lanlink::protocol::LocalCommand::leave;
    std::vector<std::byte> confirmation_payload_;
    std::string confirmation_text_;
    std::chrono::steady_clock::time_point next_diagnostics_{};
    std::chrono::steady_clock::time_point last_diagnostics_time_{};
    std::uint64_t last_diagnostics_generation_ = 0;
    int game_profile_ = -1;
    std::uint64_t last_sent_bytes_ = 0;
    std::uint64_t last_received_bytes_ = 0;
    bool last_tcp_ = false;
    bool last_connected_ = false;
    std::uint32_t last_reconnects_ = 0;
    double send_rate_ = 0;
    double receive_rate_ = 0;
};
}
