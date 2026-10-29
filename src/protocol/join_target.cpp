#include "lanlink/protocol/join_target.hpp"
#include <algorithm>
#include <stdexcept>
namespace lanlink::protocol {
namespace {
unsigned digit(char c) {
    if(c>='0' && c<='9') return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    throw std::invalid_argument("invalid join identifier");
}
std::string hex(std::span<const std::byte> bytes) {
    constexpr char digits[]="0123456789abcdef";std::string out;
    for(auto b:bytes){const auto v=std::to_integer<unsigned>(b);out+=digits[v>>4];out+=digits[v&15];}return out;
}
}
JoinTarget parse_join_target(std::string_view text) {
    while(!text.empty() && (text.front()==' ' || text.front()=='\r' || text.front()=='\n' || text.front()=='\t'))text.remove_prefix(1);
    while(!text.empty() && (text.back()==' ' || text.back()=='\r' || text.back()=='\n' || text.back()=='\t'))text.remove_suffix(1);
    char separator=0;
    if(text.starts_with("lanlink://join/")){text.remove_prefix(15);separator='/';}
    else if(text.starts_with("LL1.")){text.remove_prefix(4);separator='.';}
    if((!separator && text.size()!=32) || (separator && (text.size()!=73 || text[32]!=separator)))
        throw std::invalid_argument("Enter a network ID, LanLink invitation link or invitation code.");
    JoinTarget out;
    for(std::size_t i=0;i<16;++i)out.network_id[i]=static_cast<std::byte>((digit(text[i*2])<<4)|digit(text[i*2+1]));
    if(std::all_of(out.network_id.begin(),out.network_id.end(),[](auto b){return b==std::byte{0};})) throw std::invalid_argument("zero network ID");
    if(separator){out.invitation=std::string(text.substr(33));for(auto& c:out.invitation){const auto v=digit(c);c="0123456789abcdef"[v];}}
    return out;
}
std::string make_invite_link(const NetworkId& network,std::string_view secret) {
    const auto result="lanlink://join/"+hex(network)+"/"+std::string(secret);
    static_cast<void>(parse_join_target(result));return result;
}
std::string make_invite_code(const NetworkId& network,std::string_view secret) {
    const auto result="LL1."+hex(network)+"."+std::string(secret);
    static_cast<void>(parse_join_target(result));return result;
}
}
