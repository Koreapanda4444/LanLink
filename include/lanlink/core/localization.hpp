#pragma once
#include <span>
#include <string_view>
namespace lanlink::core {
enum class UiLanguage { english, korean };
struct Translation { std::string_view english; std::string_view korean; };
[[nodiscard]] std::span<const Translation> ui_translations() noexcept;
[[nodiscard]] std::string_view localized_text(UiLanguage language,std::string_view text) noexcept;
}
