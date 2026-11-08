#include "lanlink/client/profile_store.hpp"
#include "lanlink/auth/access_secret.hpp"
#include <iostream>
#include <fstream>
#include <stdexcept>
using namespace lanlink;
void check(bool b){if(!b)throw std::runtime_error("Server profile isolation failed");}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid server profile accepted");}
int main(){try{
 const auto dir=std::filesystem::temp_directory_path()/("lanlink-profiles-"+auth::random_access_code());
 struct Cleanup{std::filesystem::path p;~Cleanup(){std::error_code e;std::filesystem::remove_all(p,e);}} cleanup{dir};
 client::ProfileStore store(dir);protocol::ServerProfile a;a.name="Home";a.host="127.0.0.1";a.public_key_pin=std::string(64,'1');
 const auto secret=auth::random_access_code();const auto id=store.save(a,secret);auto b=a;b.name="Friend";b.host="friend.example";const auto other=store.save(b);
 store.select(id);check(store.state().selected_id==id);check(store.identity_path(id)!=store.identity_path(other));check(store.credential_path(other).empty());
 const auto proof=auth::AuthToken::load(store.credential_path(id)).proof({});check(auth::AuthToken::from_secret(secret).verify({},proof));
 {client::ProfileStore restored(dir);check(restored.state()==store.state());check(restored.credential_path(id)==store.credential_path(id));}
 const auto old_other=store.get(other);a=store.get(id).profile;a.name="Renamed";static_cast<void>(store.save(a));check(!store.credential_path(id).empty());check(store.get(other).profile==old_other.profile);
 const auto old_identity=store.identity_path(id);a.host="different.example";static_cast<void>(store.save(a));check(store.credential_path(id).empty());check(store.identity_path(id)!=old_identity);
 a.host="bad.example/command";rejects([&]{static_cast<void>(store.save(a));});check(store.state().profiles.size()==2);
 auto state=store.state();check(protocol::decode_server_state(protocol::encode_server_state(state))==state);
 protocol::ServerInvitation invite;invite.server=b;invite.registration_code=auth::random_access_code();invite.network_id[0]=std::byte{1};invite.network_invitation=auth::random_access_code();
 const auto uri=protocol::make_server_invitation(invite);check(protocol::parse_server_invitation(uri)==invite);rejects([&]{static_cast<void>(protocol::parse_server_invitation(uri+";command"));});rejects([&]{static_cast<void>(protocol::parse_server_invitation("lanlink://connect/"+std::string(3000,'a')));});
 store.erase(id);check(store.state().selected_id.empty());check(store.get(other).profile==old_other.profile);store.select(other);store.select("");check(client::ProfileStore(dir).state().selected_id.empty());
 std::cout<<"Server profiles: isolated identities/credentials/trust, endpoint re-enrollment, persistence, selection, deletion, strict invitation parsing passed\n";
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
