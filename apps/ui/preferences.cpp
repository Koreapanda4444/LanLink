#include "preferences.hpp"
#include <windows.h>
#include <stdexcept>
namespace lanlink::ui {
Preferences load_preferences() {
    Preferences result;
    result.language=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_KOREAN ? core::UiLanguage::korean : core::UiLanguage::english;
    DWORD language=0,size=sizeof(language);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\LanLink",L"UiLanguage",RRF_RT_REG_DWORD,nullptr,&language,&size)==ERROR_SUCCESS)
        result.language=language==1 ? core::UiLanguage::korean : core::UiLanguage::english;
    return result;
}
void save_preferences(const Preferences& preferences) {
    HKEY key=nullptr;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\LanLink",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)
        throw std::runtime_error("Cannot save desktop preferences.");
    const DWORD language=preferences.language==core::UiLanguage::korean ? 1 : 0;
    const auto first=RegSetValueExW(key,L"UiLanguage",0,REG_DWORD,reinterpret_cast<const BYTE*>(&language),sizeof(language));
    RegCloseKey(key);
    if(first!=ERROR_SUCCESS)throw std::runtime_error("Cannot save desktop preferences.");
}
}
