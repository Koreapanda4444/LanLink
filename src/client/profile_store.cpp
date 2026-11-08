#include "lanlink/client/profile_store.hpp"
#include "lanlink/auth/access_secret.hpp"
#include "lanlink/protocol/binary.hpp"
#include "../auth/secret_store.hpp"
#include <fstream>
#include <limits>
namespace lanlink::client {
namespace {
void put(protocol::BinaryWriter& w,std::string_view s){if(s.size()>128)throw std::invalid_argument("Invalid private profile field");w.put<std::uint16_t>(static_cast<std::uint16_t>(s.size()));w.append({reinterpret_cast<const std::byte*>(s.data()),s.size()});}
std::string get_text(protocol::BinaryReader& r){const auto n=r.get<std::uint16_t>();if(n>128)throw std::invalid_argument("Invalid private profile field");const auto s=r.bytes(n);return {reinterpret_cast<const char*>(s.data()),s.size()};}
bool identifier(std::string_view s){return s.size()==40 && s.find_first_not_of("0123456789abcdef")==s.npos;}
}
ProfileStore::ProfileStore(std::filesystem::path directory):directory_(std::move(directory)) {
    const auto file=directory_/"servers.state";
    if(!std::filesystem::exists(file))return;
    const auto data=auth::detail::read_private_blob(file);protocol::BinaryReader r(data);
    if(r.get<std::uint32_t>()!=0x4c4c5031)throw std::runtime_error("Invalid server profile store");
    const auto n=r.get<std::uint32_t>();const auto state=protocol::decode_server_state(r.bytes(n));
    revision_=state.revision;epoch_=state.connection_epoch;selected_=state.selected_id;
    for(auto profile:state.profiles){auto identity=get_text(r),credential=get_text(r);if(!identifier(identity) || (!credential.empty()&&!identifier(credential)))throw std::runtime_error("Invalid profile credential reference");auto id=profile.id;profiles_.emplace(id,StoredProfile{std::move(profile),std::move(identity),std::move(credential)});}
    r.finish();
}
protocol::ServerState ProfileStore::state()const {protocol::ServerState s;s.revision=revision_;s.connection_epoch=epoch_;s.selected_id=selected_;for(const auto& [id,p]:profiles_){static_cast<void>(id);s.profiles.push_back(p.profile);}return s;}
void ProfileStore::persist(const std::map<std::string,StoredProfile>& profiles,std::string_view selected,std::uint64_t revision,std::uint64_t epoch) {
    if(revision==std::numeric_limits<std::uint64_t>::max())throw std::runtime_error("Profile revision exhausted");
    protocol::ServerState s;s.revision=revision;s.connection_epoch=epoch;s.selected_id=selected;for(const auto& [id,p]:profiles){static_cast<void>(id);s.profiles.push_back(p.profile);}
    const auto encoded=protocol::encode_server_state(s);protocol::BinaryWriter w;w.put<std::uint32_t>(0x4c4c5031);w.put<std::uint32_t>(static_cast<std::uint32_t>(encoded.size()));w.append(encoded);
    for(const auto& [id,p]:profiles){static_cast<void>(id);put(w,p.identity);put(w,p.credential);}
    auth::detail::write_private_blob(directory_/"servers.state",std::move(w).finish());
}
StoredProfile ProfileStore::get(std::string_view id)const {const auto i=profiles_.find(std::string(id));if(i==profiles_.end())throw std::invalid_argument("Server profile not found");return i->second;}
std::filesystem::path ProfileStore::identity_path(std::string_view id)const{return directory_/std::string(id)/(get(id).identity+".identity");}
std::filesystem::path ProfileStore::credential_path(std::string_view id)const{const auto p=get(id);return p.credential.empty()?std::filesystem::path{}:directory_/std::string(id)/(p.credential+".credential");}
std::string ProfileStore::save(protocol::ServerProfile p,std::string_view credential) {
    protocol::validate_server_profile(p);
    if(p.id.empty())p.id=auth::random_access_code();
    auto next=profiles_;const auto previous=next.find(p.id);StoredProfile record{p,auth::random_access_code(),{}};
    if(previous!=next.end()) {
        record=previous->second;
        if(record.profile.host!=p.host || record.profile.port!=p.port || record.profile.public_key_pin!=p.public_key_pin){record.identity=auth::random_access_code();record.credential.clear();}
        record.profile=p;
    } else if(next.size()>=32)throw std::invalid_argument("At most 32 server profiles may be saved");
    std::filesystem::path created;
    try {
        if(!credential.empty()){
            record.credential=auth::random_access_code();created=directory_/p.id/(record.credential+".credential");
            auth::AuthToken::store_credential(created,credential);
        }
        record.profile.has_credential=!record.credential.empty();
        const bool changes_connection=selected_==p.id && (previous==next.end() || previous->second.identity!=record.identity || previous->second.credential!=record.credential);
        next[p.id]=record;
        const auto epoch=epoch_+(changes_connection?1U:0U);
        persist(next,selected_,revision_+1,epoch);profiles_=std::move(next);++revision_;epoch_=epoch;
    }catch(...){if(!created.empty()){std::error_code ignored;std::filesystem::remove(created,ignored);}throw;}
    return p.id;
}
void ProfileStore::save_device_credential(std::string_view id,std::string_view secret){static_cast<void>(save(get(id).profile,secret));}
void ProfileStore::erase(std::string_view id){static_cast<void>(get(id));auto next=profiles_;next.erase(std::string(id));auto selected=selected_==id?std::string{}:selected_;const auto epoch=epoch_+(selected_==id?1U:0U);persist(next,selected,revision_+1,epoch);profiles_=std::move(next);selected_=selected;++revision_;epoch_=epoch;std::error_code ignored;std::filesystem::remove_all(directory_/std::string(id),ignored);}
void ProfileStore::select(std::string_view id){if(!id.empty())static_cast<void>(get(id));persist(profiles_,id,revision_+1,epoch_+1);selected_=id;++revision_;++epoch_;}
}
