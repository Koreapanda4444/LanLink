#include "preferences.hpp"
#include <windows.h>
#include <stdexcept>
namespace lanlink::ui {
Preferences load_preferences() {
    Preferences result;
    result.language=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_KOREAN ? core::UiLanguage::korean : core::UiLanguage::english;
    DWORD language=0,size=sizeof(language),completed=0;
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\LanLink",L"UiLanguage",RRF_RT_REG_DWORD,nullptr,&language,&size)==ERROR_SUCCESS)
        result.language=language==1 ? core::UiLanguage::korean : core::UiLanguage::english;
    size=sizeof(completed);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\LanLink",L"SetupCompleted",RRF_RT_REG_DWORD,nullptr,&completed,&size)==ERROR_SUCCESS)
        result.setup_completed=completed==1;
    return result;
}
void save_preferences(const Preferences& preferences) {
    HKEY key=nullptr;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\LanLink",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)
        throw std::runtime_error("Cannot save desktop preferences.");
    const DWORD language=preferences.language==core::UiLanguage::korean ? 1 : 0,completed=preferences.setup_completed ? 1 : 0;
    const auto first=RegSetValueExW(key,L"UiLanguage",0,REG_DWORD,reinterpret_cast<const BYTE*>(&language),sizeof(language));
    const auto second=RegSetValueExW(key,L"SetupCompleted",0,REG_DWORD,reinterpret_cast<const BYTE*>(&completed),sizeof(completed));
    RegCloseKey(key);
    if(first!=ERROR_SUCCESS || second!=ERROR_SUCCESS)throw std::runtime_error("Cannot save desktop preferences.");
}
}
