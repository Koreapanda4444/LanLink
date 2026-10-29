#include "lanlink/auth/directory.hpp"
#include "lanlink/auth/access_secret.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace lanlink;
void require(bool ok,const char* s){if(!ok)throw std::runtime_error(s);}
template<class F> void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
int main(){
    const auto path=std::filesystem::temp_directory_path()/auth::random_access_code();
    try{
        auto id=auth::DeviceIdentity::load_or_create(path);
        auth::DirectoryCatalog c{1000,61000,{{"local","127.0.0.1",4433},{"eu","relay.example.com",4434}}};
        auto bytes=auth::sign_directory(c,id);
        require(auth::verify_directory(bytes,id.public_key(),1000).relays==c.relays,"signed directory round trip");
        rejects([&]{static_cast<void>(auth::verify_directory(bytes,id.public_key(),61000));});
        rejects([&]{static_cast<void>(auth::verify_directory(bytes,id.public_key(),999));});
        auto altered=bytes;altered[25]^=std::byte{1};rejects([&]{static_cast<void>(auth::verify_directory(altered,id.public_key(),1000));});
        auto wrong=id.public_key();wrong[0]^=std::byte{1};rejects([&]{static_cast<void>(auth::verify_directory(bytes,wrong,1000));});
        core::RuntimeConfig config; config.relay_host="manual.example.com";
        bool fetched=false;
        auto fetch=[&](const std::string&){fetched=true;return bytes;};
        require(auth::resolve_bootstrap(config,fetch,1000).relay.host==config.relay_host && !fetched,"manual override never fetches directory");
        config.bootstrap_mode="self_hosted";config.directory_url="https://localhost/v1/relays";
        config.directory_public_key=auth::hex_encode(id.public_key());config.directory_relay_id="eu";
        core::validate_config(config);
        require(auth::resolve_bootstrap(config,fetch,1000).relay.host=="relay.example.com","authenticated directory selection");
        bytes=altered;auto fallback=auth::resolve_bootstrap(config,fetch,1000);
        require(!fallback.from_directory && fallback.relay.host==config.relay_host && !fallback.error.empty(),"untrusted directory preserves manual fallback");
        config.directory_url="http://localhost/v1/relays";rejects([&]{core::validate_config(config);});
        std::filesystem::remove(path);std::cout<<"signed bootstrap passed\n";return 0;
    }catch(const std::exception& e){std::filesystem::remove(path);std::cerr<<e.what()<<'\n';return 1;}
}
