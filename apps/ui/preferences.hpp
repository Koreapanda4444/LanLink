#pragma once
#include "lanlink/core/localization.hpp"
namespace lanlink::ui {
struct Preferences { core::UiLanguage language=core::UiLanguage::english; };
[[nodiscard]] Preferences load_preferences();
void save_preferences(const Preferences& preferences);
}
