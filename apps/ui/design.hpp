#pragma once
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace lanlink::ui::design {
inline constexpr int default_width = 500;
inline constexpr int default_height = 750;
inline ImFont* emphasis_font = nullptr;
inline ImFont* title_font = nullptr;
inline constexpr ImVec4 ink{.12f,.16f,.22f,1};
inline constexpr ImVec4 muted{.43f,.48f,.55f,1};
inline constexpr ImVec4 accent{.16f,.36f,.75f,1};
inline constexpr ImVec4 border{.86f,.89f,.92f,1};
inline constexpr ImVec4 green{.10f,.48f,.32f,1};

inline bool button(const char* text,ImVec2 size=ImVec2(0,34),bool primary=false) {
    (void)primary;
    return ImGui::Button(text,size);
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
    (void)online;
    ImGui::TextColored(error?ImVec4(.65f,.12f,.12f,1):ink,"%s",label);
}
inline void load_fonts(const std::filesystem::path& regular,const std::filesystem::path& bold) {
    const auto* ranges=ImGui::GetIO().Fonts->GetGlyphRangesKorean();
    const auto face=std::filesystem::is_regular_file(bold)?bold:regular;
    if(auto* font=ImGui::GetIO().Fonts->AddFontFromFileTTF(regular.string().c_str(),16,nullptr,ranges))ImGui::GetIO().FontDefault=font;
    emphasis_font=ImGui::GetIO().Fonts->AddFontFromFileTTF(face.string().c_str(),16,nullptr,ranges);
    title_font=emphasis_font;
}
}
