#pragma once
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace lanlink::ui::design {
inline constexpr int default_width = 700;
inline constexpr int default_height = 1050;
inline ImFont* emphasis_font = nullptr;
inline ImFont* title_font = nullptr;
inline constexpr ImVec4 ink{.12f,.16f,.22f,1};
inline constexpr ImVec4 muted{.43f,.48f,.55f,1};
inline constexpr ImVec4 accent{.16f,.36f,.75f,1};
inline constexpr ImVec4 border{.86f,.89f,.92f,1};
inline constexpr ImVec4 green{.10f,.48f,.32f,1};

enum class Icon { settings, power, more, network, copy, plus, chevron };
inline void icon(Icon kind, ImVec2 p, float size, ImU32 color) {
    auto* draw=ImGui::GetWindowDrawList();
    const auto point=[&](float x,float y){return ImVec2(p.x+x*size,p.y+y*size);};
    const auto line=[&](float x,float y,float u,float v){draw->AddLine(point(x,y),point(u,v),color,1.6f);};
    if(kind==Icon::power) {
        draw->PathArcTo(point(.5f,.53f),size*.32f,-.95f,4.09f,24);draw->PathStroke(color,0,1.7f);
        line(.5f,.12f,.5f,.48f);
    } else if(kind==Icon::chevron) {line(.3f,.42f,.5f,.62f);line(.5f,.62f,.7f,.42f);}
    else if(kind==Icon::plus) {line(.5f,.22f,.5f,.78f);line(.22f,.5f,.78f,.5f);}
    else if(kind==Icon::more) {for(float x:{.25f,.5f,.75f})draw->AddCircleFilled(point(x,.5f),size*.06f,color);}
    else if(kind==Icon::copy) {
        draw->AddRect(point(.34f,.3f),point(.78f,.82f),color,2,0,1.5f);
        line(.22f,.64f,.22f,.18f);line(.22f,.18f,.64f,.18f);
    } else if(kind==Icon::settings) {
        draw->AddCircle(point(.5f,.5f),size*.27f,color,20,1.6f);
        draw->AddCircle(point(.5f,.5f),size*.09f,color,12,1.5f);
        for(int i=0;i<8;++i){const float a=i*3.14159265f/4;line(.5f+.27f*std::cos(a),.5f+.27f*std::sin(a),.5f+.39f*std::cos(a),.5f+.39f*std::sin(a));}
    } else {
        line(.5f,.3f,.5f,.53f);line(.2f,.53f,.8f,.53f);line(.2f,.53f,.2f,.68f);line(.8f,.53f,.8f,.68f);
        for(const auto center:{ImVec2(.5f,.2f),ImVec2(.2f,.78f),ImVec2(.8f,.78f)})
            draw->AddRect(point(center.x-.11f,center.y-.09f),point(center.x+.11f,center.y+.09f),color,2,0,1.6f);
    }
}
inline bool icon_button(const char* id,Icon kind,const char* tooltip, bool on=false) {
    const auto p=ImGui::GetCursorScreenPos();
    const bool pressed=ImGui::InvisibleButton(id,ImVec2(34,34),ImGuiButtonFlags_EnableNav);
    const bool hover=ImGui::IsItemHovered();
    auto* draw=ImGui::GetWindowDrawList();
    if(hover || on)draw->AddRectFilled(p,ImVec2(p.x+34,p.y+34),ImGui::GetColorU32(on?ImVec4(.91f,.97f,.94f,1):ImVec4(.92f,.94f,.96f,1)),7);
    if(ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)draw->AddRect(p,ImVec2(p.x+34,p.y+34),ImGui::GetColorU32(accent),7);
    icon(kind,ImVec2(p.x+7,p.y+7),20,ImGui::GetColorU32(on?green:muted));
    if(hover && tooltip && *tooltip)ImGui::SetTooltip("%s",tooltip);
    return pressed;
}
inline bool button(const char* text,ImVec2 size=ImVec2(0,34),bool primary=false) {
    if(primary) {
        ImGui::PushStyleColor(ImGuiCol_Button,accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.12f,.30f,.66f,1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(.09f,.24f,.56f,1));
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,1,1,1));
        ImGui::PushStyleColor(ImGuiCol_Border,accent);
    }
    const bool result=ImGui::Button(text,size);
    if(primary)ImGui::PopStyleColor(5);
    return result;
}
inline void title(const char* text) {
    if(title_font)ImGui::PushFont(title_font);
    ImGui::TextUnformatted(text);
    if(title_font)ImGui::PopFont();
}
inline void emphasis(const char* text) {
    if(emphasis_font)ImGui::PushFont(emphasis_font);
    ImGui::TextUnformatted(text);
    if(emphasis_font)ImGui::PopFont();
}
inline void status(const char* label,bool online,bool error=false) {
    const auto p=ImGui::GetCursorScreenPos();
    const auto text=ImGui::CalcTextSize(label);
    const auto tint=online?green:error?ImVec4(.69f,.25f,.20f,1):muted;
    const auto bg=online?ImVec4(.91f,.97f,.94f,1):error?ImVec4(.99f,.94f,.92f,1):ImVec4(.92f,.94f,.96f,1);
    auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(p,ImVec2(p.x+text.x+30,p.y+26),ImGui::GetColorU32(bg),13);
    draw->AddCircleFilled(ImVec2(p.x+12,p.y+13),3,ImGui::GetColorU32(tint));
    draw->AddText(ImVec2(p.x+21,p.y+(26-text.y)*.5f),ImGui::GetColorU32(tint),label);
    ImGui::Dummy(ImVec2(text.x+30,26));
}
inline void load_fonts(const std::filesystem::path& regular,const std::filesystem::path& bold) {
    const auto* ranges=ImGui::GetIO().Fonts->GetGlyphRangesKorean();
    const auto face=std::filesystem::is_regular_file(bold)?bold:regular;
    if(auto* font=ImGui::GetIO().Fonts->AddFontFromFileTTF(regular.string().c_str(),16,nullptr,ranges))ImGui::GetIO().FontDefault=font;
    emphasis_font=ImGui::GetIO().Fonts->AddFontFromFileTTF(face.string().c_str(),16,nullptr,ranges);
    title_font=ImGui::GetIO().Fonts->AddFontFromFileTTF(face.string().c_str(),22,nullptr,ranges);
}
}
