#include "preferences.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
namespace lanlink::ui {
namespace {
std::filesystem::path path() {
    const auto* home=std::getenv("HOME");
    if(!home || !std::filesystem::path(home).is_absolute())throw std::runtime_error("cannot locate desktop preferences");
#ifdef __APPLE__
    return std::filesystem::path(home)/"Library/Application Support/LanLink/ui.conf";
#else
    const auto* xdg=std::getenv("XDG_CONFIG_HOME");
    if(xdg && std::filesystem::path(xdg).is_absolute())return std::filesystem::path(xdg)/"lanlink/ui.conf";
    return std::filesystem::path(home)/".config/lanlink/ui.conf";
#endif
}
}
Preferences load_preferences() {
    Preferences result;
    const char* locale=std::getenv("LC_ALL");if(!locale || !*locale)locale=std::getenv("LC_MESSAGES");if(!locale || !*locale)locale=std::getenv("LANG");
    if(locale && std::string_view(locale).starts_with("ko"))result.language=core::UiLanguage::korean;
    std::ifstream input(path());std::string line;
    while(std::getline(input,line)) {
        if(line=="language=ko")result.language=core::UiLanguage::korean;
        else if(line=="language=en")result.language=core::UiLanguage::english;
        else if(line=="setup_completed=1")result.setup_completed=true;
    }
    return result;
}
void save_preferences(const Preferences& value) {
    const auto target=path();std::filesystem::create_directories(target.parent_path());
    const auto temporary=target.string()+".tmp-"+std::to_string(getpid());
    try {
        std::ofstream output(temporary,std::ios::trunc);
        output<<"language="<<(value.language==core::UiLanguage::korean?"ko":"en")<<"\nsetup_completed="<<value.setup_completed<<'\n';
        output.close();if(!output)throw std::runtime_error("cannot save desktop preferences");
        if(chmod(temporary.c_str(),0600)<0)throw std::runtime_error("cannot protect desktop preferences");
        std::filesystem::rename(temporary,target);
    }catch(...){std::filesystem::remove(temporary);throw;}
}
}
