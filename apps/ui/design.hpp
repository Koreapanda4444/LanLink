#pragma once
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace lanlink::ui::design {
inline constexpr int default_width = 500;
inline constexpr int default_height = 750;
inline float dpi_scale = 1.0f;
inline float px(float value) { return value * dpi_scale; }
inline ImVec2 size(float x,float y) { return ImVec2(px(x),px(y)); }
inline ImFont* emphasis_font = nullptr;
inline ImFont* title_font = nullptr;
inline constexpr ImVec4 ink{.91f,.95f,1,1};
inline constexpr ImVec4 muted{.56f,.65f,.76f,1};
inline constexpr ImVec4 accent{.08f,.62f,.96f,1};
inline constexpr ImVec4 border{.16f,.22f,.31f,1};
inline constexpr ImVec4 green{.24f,.84f,.62f,1};
inline constexpr ImVec4 surface{.075f,.10f,.15f,1};
inline constexpr ImVec4 raised{.11f,.15f,.22f,1};

inline bool button(const char* text,ImVec2 dimensions=ImVec2(0,34),bool primary=false) {
    if(primary) {
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.04f,.36f,.65f,1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.06f,.46f,.79f,1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(.03f,.28f,.52f,1));
    }
    const auto pressed=ImGui::Button(text,size(dimensions.x,dimensions.y));
    if(primary)ImGui::PopStyleColor(3);
    return pressed;
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
    ImGui::TextColored(error?ImVec4(1,.42f,.40f,1):online?green:muted,"%s",label);
}
inline void load_fonts(const std::filesystem::path& regular,const std::filesystem::path& bold,float density=1) {
    auto& io=ImGui::GetIO();
    ImFontConfig config;
    config.OversampleH=2; config.OversampleV=2; config.PixelSnapH=true;
    const auto font_size=std::round(16*dpi_scale*density);
    io.FontGlobalScale=1/density;
    const auto* ranges=ImGui::GetIO().Fonts->GetGlyphRangesKorean();
    const auto face=std::filesystem::is_regular_file(bold)?bold:regular;
    if(auto* font=ImGui::GetIO().Fonts->AddFontFromFileTTF(regular.string().c_str(),font_size,&config,ranges))ImGui::GetIO().FontDefault=font;
    emphasis_font=ImGui::GetIO().Fonts->AddFontFromFileTTF(face.string().c_str(),font_size,&config,ranges);
    title_font=emphasis_font;
}
}
