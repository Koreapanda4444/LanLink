#pragma once
#include "lanlink/core/localization.hpp"
namespace lanlink::ui {
struct Preferences { core::UiLanguage language=core::UiLanguage::english; bool setup_completed=false; };
[[nodiscard]] Preferences load_preferences();
void save_preferences(const Preferences& preferences);
}
