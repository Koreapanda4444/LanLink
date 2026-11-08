#include "lanlink/protocol/server_profiles.hpp"
#include "lanlink/protocol/binary.hpp"
#include <algorithm>
#include <cctype>
#include <set>
namespace lanlink::protocol {
namespace {
void text(BinaryWriter& w, std::string_view s, std::size_t limit) {
    if(s.size()>limit || s.find('\0')!=s.npos)throw std::invalid_argument("Server field exceeds its limit");
    w.put<std::uint16_t>(static_cast<std::uint16_t>(s.size()));
    w.append({reinterpret_cast<const std::byte*>(s.data()),s.size()});
}
std::string text(BinaryReader& r,std::size_t limit) {
    const auto n=r.get<std::uint16_t>();if(n>limit)throw std::invalid_argument("Server field exceeds its limit");
    const auto s=r.bytes(n);std::string out(reinterpret_cast<const char*>(s.data()),s.size());
    if(out.find('\0')!=out.npos)throw std::invalid_argument("Invalid server text");
    return out;
}
bool hex(std::string_view s,std::size_t n){return s.size()==n && std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
void put_profile(BinaryWriter& w,const ServerProfile& p){text(w,p.id,40);text(w,p.name,128);text(w,p.host,253);w.put(p.port);text(w,p.public_key_pin,64);w.put<std::uint8_t>(p.has_credential?1:0);}
ServerProfile get_profile(BinaryReader& r){ServerProfile p;p.id=text(r,40);p.name=text(r,128);p.host=text(r,253);p.port=r.get<std::uint16_t>();p.public_key_pin=text(r,64);const auto flag=r.get<std::uint8_t>();if(flag>1)throw std::invalid_argument("Invalid credential flag");p.has_credential=flag!=0;return p;}
}
void validate_server_profile(const ServerProfile& p,bool require_id) {
    if((require_id || !p.id.empty()) && !hex(p.id,40))throw std::invalid_argument("Invalid server profile ID");
    if(p.name.empty() || p.name.size()>128 || std::any_of(p.name.begin(),p.name.end(),[](unsigned char c){return c<32 || c==127;}))throw std::invalid_argument("Enter a server name");
    if(p.host.empty() || p.host.size()>253 || !p.port || !std::all_of(p.host.begin(),p.host.end(),[](unsigned char c){return std::isalnum(c) || c=='.' || c=='-' || c==':';}) || p.host.front()=='-' || p.host=="." || p.host==":")throw std::invalid_argument("Enter a hostname or IP address and a valid port");
    if(!p.public_key_pin.empty() && (!hex(p.public_key_pin,64) || p.public_key_pin==std::string(64,'0')))throw std::invalid_argument("Enter a SHA-256 public key fingerprint (64 lowercase hexadecimal characters)");
}
std::vector<std::byte> encode_server_request(const ServerRequest& q) {
    if(q.action>ServerAction::enroll)throw std::invalid_argument("Unknown server action");
    BinaryWriter w;w.put<std::uint8_t>(static_cast<std::uint8_t>(q.action));put_profile(w,q.profile);text(w,q.credential,4096);text(w,q.registration_code,40);text(w,q.device_label,128);return std::move(w).finish();
}
ServerRequest decode_server_request(std::span<const std::byte> b){BinaryReader r(b);ServerRequest q;q.action=static_cast<ServerAction>(r.get<std::uint8_t>());if(q.action>ServerAction::enroll)throw std::invalid_argument("Unknown server action");q.profile=get_profile(r);q.credential=text(r,4096);q.registration_code=text(r,40);q.device_label=text(r,128);r.finish();return q;}
std::vector<std::byte> encode_server_state(const ServerState& s) {
    if(s.profiles.size()>32)throw std::invalid_argument("Too many server profiles");
    BinaryWriter w;w.put(s.revision);w.put(s.connection_epoch);text(w,s.selected_id,40);w.put<std::uint8_t>(static_cast<std::uint8_t>(s.profiles.size()));
    std::set<std::string> ids;for(const auto& p:s.profiles){validate_server_profile(p,true);if(!ids.insert(p.id).second)throw std::invalid_argument("Duplicate server profile");put_profile(w,p);}
    if(!s.selected_id.empty() && !ids.contains(s.selected_id))throw std::invalid_argument("Selected server is absent");
    return std::move(w).finish();
}
ServerState decode_server_state(std::span<const std::byte> b){BinaryReader r(b);ServerState s;s.revision=r.get<std::uint64_t>();s.connection_epoch=r.get<std::uint64_t>();s.selected_id=text(r,40);const auto n=r.get<std::uint8_t>();if(n>32)throw std::invalid_argument("Too many server profiles");for(unsigned i=0;i<n;++i)s.profiles.push_back(get_profile(r));r.finish();static_cast<void>(encode_server_state(s));return s;}
std::string make_server_invitation(const ServerInvitation& i) {
    validate_server_profile(i.server);
    if((!i.registration_code.empty()&&!hex(i.registration_code,40)) || (!i.network_invitation.empty()&&!hex(i.network_invitation,40)))throw std::invalid_argument("Invalid invitation code");
    if(!i.network_invitation.empty() && std::all_of(i.network_id.begin(),i.network_id.end(),[](auto b){return b==std::byte{0};}))throw std::invalid_argument("Missing invitation network");
    auto p=i.server;p.id.clear();p.has_credential=false;BinaryWriter w;w.put<std::uint8_t>(1);put_profile(w,p);text(w,i.registration_code,40);w.append(i.network_id);text(w,i.network_invitation,40);
    std::string uri="lanlink://connect/";for(const auto b:std::move(w).finish()){const auto n=std::to_integer<unsigned>(b);uri+="0123456789abcdef"[n>>4];uri+="0123456789abcdef"[n&15];}return uri;
}
ServerInvitation parse_server_invitation(std::string_view uri) {
    constexpr std::string_view prefix="lanlink://connect/";if(!uri.starts_with(prefix) || uri.size()>2048)throw std::invalid_argument("Invalid server invitation link");uri.remove_prefix(prefix.size());
    if(uri.empty() || uri.size()%2 || !hex(uri,uri.size()))throw std::invalid_argument("Invalid invitation encoding");
    const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};std::vector<std::byte> b;for(std::size_t n=0;n<uri.size();n+=2)b.push_back(static_cast<std::byte>((digit(uri[n])<<4)|digit(uri[n+1])));
    BinaryReader r(b);if(r.get<std::uint8_t>()!=1)throw std::invalid_argument("Unsupported invitation version");ServerInvitation i;i.server=get_profile(r);i.registration_code=text(r,40);i.network_id=r.array<16>();i.network_invitation=text(r,40);r.finish();
    if(!i.server.id.empty() || i.server.has_credential)throw std::invalid_argument("Invitation cannot contain local credentials");
    static_cast<void>(make_server_invitation(i));return i;
}
}
