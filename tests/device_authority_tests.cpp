#include "lanlink/relay/device_authority.hpp"
#include "lanlink/auth/handshake.hpp"
#include <iostream>
using namespace lanlink;
void check(bool b){if(!b)throw std::runtime_error("Device authority isolation failed");}
protocol::DeviceRegistration request(const auth::DeviceIdentity& id,std::string code){protocol::DeviceRegistration q;q.public_key=id.public_key();q.nonce=auth::random_nonce();q.code=std::move(code);q.label="Test device";q.signature=id.sign(protocol::registration_transcript(q));return q;}
bool authenticate(relay::DeviceAuthority& authority,const auth::DeviceIdentity& id,const auth::AuthToken& secret){auto dummy=auth::AuthToken::from_secret(std::string(40,'x'));auth::ServerHandshake server(dummy,1,auth::ServerHandshake::Clock::now()+std::chrono::seconds(5),[&](const auto& key,auto transcript,const auto& proof){return authority.verify(key,transcript,proof);});auth::ClientHandshake client(id,secret);return client.handle_result(server.handle_proof(client.handle_challenge(server.handle_hello(client.begin(1)))));}
int main(){try{
 auto dir=std::filesystem::temp_directory_path()/("lanlink-authority-"+auth::random_access_code());std::filesystem::create_directories(dir);
 struct Cleanup{std::filesystem::path p;~Cleanup(){std::error_code e;std::filesystem::remove_all(p,e);}} cleanup{dir};
 storage::RelayStore store(dir/"relay.db");auto master=auth::AuthToken::from_secret(std::string(64,'a'));relay::DeviceAuthority authority(store,master,true),legacy(store,master,false);
 const auto alice=auth::DeviceIdentity::load_or_create(dir/"alice.identity"),bob=auth::DeviceIdentity::load_or_create(dir/"bob.identity");
 check(!authenticate(authority,alice,master));check(authenticate(legacy,alice,master));
 const auto code=authority.issue_code(1000,3);auto q=request(alice,code);auto result=authority.register_device(protocol::encode_device_registration(q),2000);check(result.size()==65);
 const auto secret=std::string(reinterpret_cast<const char*>(result.data()+1),64);auto token=auth::AuthToken::from_secret(secret);
 check(authenticate(authority,alice,token));check(!authenticate(authority,alice,master));check(!authenticate(authority,bob,token));
 check(authority.register_device(protocol::encode_device_registration(request(bob,code)),2000)==std::vector{std::byte{0}});
 const auto expired=authority.issue_code(1000,1);check(authority.register_device(protocol::encode_device_registration(request(bob,expired)),2000)==std::vector{std::byte{0}});
 const auto revoked=authority.issue_code(2000);authority.revoke_code(revoked);check(authority.register_device(protocol::encode_device_registration(request(bob,revoked)),2001)==std::vector{std::byte{0}});
 const auto valid=authority.issue_code(2000);q=request(bob,valid);q.signature[0]^=std::byte{1};check(authority.register_device(protocol::encode_device_registration(q),2001)==std::vector{std::byte{0}});
 check(authority.register_device(protocol::encode_device_registration(request(bob,valid)),2001).size()==65);
 store.revoke_server_device(alice.device_id());check(!authority.allowed(alice.device_id()));check(!authenticate(authority,alice,token));check(!authenticate(legacy,alice,master));
 const auto fresh=authority.issue_code(3000);check(authority.register_device(protocol::encode_device_registration(request(alice,fresh)),3001)==std::vector{std::byte{0}});
 check(store.server_devices().size()==2);check(storage::RelayStore(dir/"relay.db").server_device_revoked(alice.device_id())==true);
 std::cout<<"Device authority: expiring single-use codes, proof of possession, scoped credentials, legacy isolation and persistent revocation passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
