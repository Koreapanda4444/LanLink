#include "lanlink/core/localization.hpp"
#include <iostream>
#include <regex>
#include <stdexcept>
#include <vector>
#include <string>
std::vector<std::string> formats(std::string_view s) {
    const std::string text(s);const std::regex pattern("%[-+ #0-9.*]*[hlLjzt]*[sdiuoxXfFeEgGcpaA%]");
    std::vector<std::string> result;
    for(auto i=std::sregex_iterator(text.begin(),text.end(),pattern);i!=std::sregex_iterator();++i)result.push_back(i->str());
    return result;
}
int main(){try{
    using namespace lanlink::core;
    if(ui_translations().size()<150)throw std::runtime_error("incomplete translation catalog");
    for(const auto& x:ui_translations()){
        if(x.korean.empty() || formats(x.english)!=formats(x.korean))throw std::runtime_error("translated format mismatch: "+std::string(x.english));
        if(localized_text(UiLanguage::english,x.english)!=x.english || localized_text(UiLanguage::korean,x.english)!=x.korean)throw std::runtime_error("locale selection failed");
    }
    std::cout<<ui_translations().size()<<" translations verified\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
