#include "lanlink/auth/directory.hpp"
#include "lanlink/auth/relay_health.hpp"
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
        auth::DirectoryCatalog regional=c;regional.scope=auth::DirectoryScope::self_hosted;regional.revision=7;
        for(auto& relay:regional.relays){relay.region="eu-central";relay.capacity=128;relay.authority="home";relay.health_key=id.public_key();}
        auto other=regional.relays.front();other.id="other";other.host="other.example.com";other.authority="isolated";regional.relays.push_back(other);
        bytes=auth::sign_directory(regional,id);const auto verified=auth::verify_directory(bytes,id.public_key(),1000);
        require(verified.scope==regional.scope && verified.revision==7 && verified.relays==regional.relays,"regional metadata must be authenticated and canonical");
        config.directory_relay_id.clear();auto pool=auth::resolve_bootstrap(config,fetch,1000);
        require(pool.pool.size()==2 && pool.catalog_revision==7 && pool.catalog_expires_ms==61000,"failover pool must stay within one authority");
        config.directory_relay_id="eu";require(auth::resolve_bootstrap(config,fetch,1000).pool.size()==1,"explicit relay pin disables other candidates");
        config.directory_relay_id.clear();config.bootstrap_mode="official";
        require(!auth::resolve_bootstrap(config,fetch,1000).from_directory,"self-hosted catalogs must not cross official trust scope");
        config.bootstrap_mode="self_hosted";
        regional.relays.front().protocol_version=6;bytes=auth::sign_directory(regional,id);
        require(auth::resolve_bootstrap(config,fetch,1000).relay.id=="eu","unsupported relay protocol must not be selected");
        regional.relays.front().protocol_version=protocol::protocol_version;
        auto invalid=regional;invalid.relays.front().health_key={};rejects([&]{static_cast<void>(auth::sign_directory(invalid,id));});
        invalid=regional;invalid.relays[1].host=invalid.relays[0].host;invalid.relays[1].port=invalid.relays[0].port;
        rejects([&]{static_cast<void>(auth::sign_directory(invalid,id));});
        invalid=regional;invalid.scope=auth::DirectoryScope::legacy;rejects([&]{static_cast<void>(auth::sign_directory(invalid,id));});
        invalid=regional;invalid.revision=0;rejects([&]{static_cast<void>(auth::sign_directory(invalid,id));});
        auth::RelayHealth signal;signal.nonce=auth::random_nonce();signal.relay_id=regional.relays[0].id;signal.authority="home";
        signal.issued_ms=1000;signal.capacity=128;signal.active_connections=3;signal.ready=true;signal.forwarded_bytes=8192;
        auto signed_health=auth::sign_relay_health(signal,id);
        require(auth::verify_relay_health(signed_health,regional.relays[0],signal.nonce,1000)==signal,"pinned health challenge and aggregate counters");
        rejects([&]{static_cast<void>(auth::verify_relay_health(signed_health,regional.relays[0],auth::random_nonce(),1000));});
        rejects([&]{static_cast<void>(auth::verify_relay_health(signed_health,regional.relays[0],signal.nonce,32000));});
        auto mismatched=regional.relays[0];mismatched.authority="wrong";
        rejects([&]{static_cast<void>(auth::verify_relay_health(signed_health,mismatched,signal.nonce,1000));});
        mismatched=regional.relays[0];mismatched.health_key[0]^=std::byte{1};
        rejects([&]{static_cast<void>(auth::verify_relay_health(signed_health,mismatched,signal.nonce,1000));});
        signed_health[20]^=std::byte{1};rejects([&]{static_cast<void>(auth::verify_relay_health(signed_health,regional.relays[0],signal.nonce,1000));});
        signal.active_connections=129;rejects([&]{static_cast<void>(auth::sign_relay_health(signal,id));});
        config.directory_url="http://localhost/v1/relays";rejects([&]{core::validate_config(config);});
        std::filesystem::remove(path);std::cout<<"signed bootstrap passed\n";return 0;
    }catch(const std::exception& e){std::filesystem::remove(path);std::cerr<<e.what()<<'\n';return 1;}
}
