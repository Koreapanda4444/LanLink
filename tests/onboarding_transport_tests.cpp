#include "lanlink/transport/device_registration.hpp"
#include "lanlink/transport/tls.hpp"
#include "lanlink/transport/relay_probe.hpp"
#include "lanlink/relay/device_authority.hpp"
#include "lanlink/relay/network_control_channel.hpp"
#include "lanlink/core/logger.hpp"
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <fstream>
#include <iostream>
#include <thread>
using namespace lanlink;
using namespace std::chrono_literals;
void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("Untrusted or unauthorized server access accepted");}
template<class F>void wait(F f){const auto until=std::chrono::steady_clock::now()+8s;while(!f()){check(std::chrono::steady_clock::now()<until,"Onboarding transport timed out");std::this_thread::sleep_for(10ms);}}
void certificate(const std::filesystem::path& root) {
 if(std::filesystem::is_regular_file(root/"cert.pem"))return;
 std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key(EVP_EC_gen("prime256v1"),EVP_PKEY_free);
 std::unique_ptr<X509,decltype(&X509_free)> cert(X509_new(),X509_free);check(key && cert,"certificate allocation");
 check(X509_set_version(cert.get(),2)==1 && ASN1_INTEGER_set(X509_get_serialNumber(cert.get()),1)==1 && X509_gmtime_adj(X509_getm_notBefore(cert.get()),-60) && X509_gmtime_adj(X509_getm_notAfter(cert.get()),3600) && X509_set_pubkey(cert.get(),key.get())==1,"certificate setup");
 auto* name=X509_get_subject_name(cert.get());check(X509_NAME_add_entry_by_txt(name,"CN",MBSTRING_ASC,reinterpret_cast<const unsigned char*>("127.0.0.1"),-1,-1,0)==1 && X509_set_issuer_name(cert.get(),name)==1,"certificate subject");
 for(const auto& [nid,value]:{std::pair{NID_basic_constraints,"critical,CA:FALSE"},std::pair{NID_ext_key_usage,"serverAuth"},std::pair{NID_subject_alt_name,"IP:127.0.0.1"}}){std::unique_ptr<X509_EXTENSION,decltype(&X509_EXTENSION_free)> ext(X509V3_EXT_conf_nid(nullptr,nullptr,nid,const_cast<char*>(value)),X509_EXTENSION_free);check(ext && X509_add_ext(cert.get(),ext.get(),-1)==1,"certificate extension");}
 check(X509_sign(cert.get(),key.get(),EVP_sha256())>0,"certificate signature");
 std::unique_ptr<BIO,decltype(&BIO_free)> certfile(BIO_new_file((root/"cert.pem").string().c_str(),"w"),BIO_free),keyfile(BIO_new_file((root/"key.pem").string().c_str(),"w"),BIO_free);
 check(certfile && keyfile && PEM_write_bio_X509(certfile.get(),cert.get())==1 && PEM_write_bio_PrivateKey(keyfile.get(),key.get(),nullptr,nullptr,0,nullptr,nullptr)==1,"certificate writing");
}
int main(int argc,char** argv){try{
 check(argc==4,"usage: onboarding-transport fixture PORT REGION_PORT");const std::filesystem::path root=argv[1];certificate(root);
 {std::ofstream f(root/"master.token");f<<"private-server-master-token-not-shared-with-users-2026\n";}
 storage::RelayStore store(root/"relay.db");auto master=auth::AuthToken::load(root/"master.token");relay::DeviceAuthority authority(store,master,true);
 relay::NetworkService networks(store);relay::NetworkControlChannel control(networks);core::Logger logger(root/"log","onboarding",core::LogLevel::warning,1024*1024,2);
 transport::QuicServerOptions server_options;server_options.listen_host="127.0.0.1";server_options.port=static_cast<std::uint16_t>(std::stoul(argv[2]));server_options.certificate_file=root/"cert.pem";server_options.private_key_file=root/"key.pem";server_options.auth_token_file=root/"master.token";
 server_options.verify_device=[&](const auto& k,auto t,const auto& p){return authority.verify(k,t,p);};server_options.register_device=[&](auto b){return authority.register_device(b,10000);};server_options.device_allowed=[&](const auto& id){return authority.allowed(id);};
 server_options.relay_id="authority";server_options.authority="onboarding-test";server_options.max_connections=64;server_options.health_identity_file=root/"authority.identity";
 const auto authority_key=auth::DeviceIdentity::load_or_create(server_options.health_identity_file);
 transport::QuicRelayServer server(server_options,logger,control);server.start();
 const auto identity=auth::DeviceIdentity::load_or_create(root/"device.identity");std::atomic_bool stopping=false;
 transport::QuicClientOptions options;options.relay_host="127.0.0.1";options.port=server_options.port;options.identity_file=root/"device.identity";options.auth_token_file=root/"device.credential";options.handshake_timeout=1s;options.authentication_timeout=2s;options.reconnect_initial_delay=100ms;options.reconnect_maximum_delay=200ms;
 const auto code=authority.issue_code(10000,60);auto wrong=options;wrong.server_public_key_pin=std::string(64,'1');
 rejects([&]{static_cast<void>(transport::register_device(wrong,identity,code,"Device",stopping));});check(store.server_devices().empty(),"Untrusted server received a registration");
 options.server_public_key_pin=transport::server_public_key_fingerprint(root/"cert.pem");auto name_mismatch=options;name_mismatch.relay_host="localhost";
 rejects([&]{static_cast<void>(transport::register_device(name_mismatch,identity,code,"Device",stopping));});
 const auto secret=transport::register_device(options,identity,code,"Device",stopping);check(secret.size()==64,"Individual credential missing");auth::AuthToken::store_credential(options.auth_token_file,secret);
 rejects([&]{static_cast<void>(transport::register_device(options,identity,code,"Device",stopping));});
 transport::QuicRelayClient client(options,logger);std::exception_ptr error;
 std::jthread worker([&](std::stop_token stop){try{client.run(stop);}catch(...){error=std::current_exception();}});
 wait([&]{return client.authenticated();});check(client.using_tcp_fallback(),"Pinned trust did not use the verified TLS path");
 protocol::AccessRequest account;account.action=protocol::AccessAction::account_status;check(client.access_request(account).code==protocol::NetworkResultCode::not_found,"Registered device unexpectedly required a cloud account or raced capability negotiation");
 check(client.list_networks().code==protocol::NetworkResultCode::success,"Registered device cannot list networks");
 const auto made=client.create_network("Approved network");check(made.code==protocol::NetworkResultCode::success,"Registered device cannot create a network");
 store.revoke_server_device(identity.device_id());wait([&]{return !client.authenticated();});
 worker.request_stop();client.stop();worker.join();if(error)std::rethrow_exception(error);
 // Registration through a regional TLS bridge must drain its final response before closing.
 auto region_options=server_options;region_options.port=static_cast<std::uint16_t>(std::stoul(argv[3]));region_options.relay_id="region";region_options.health_identity_file=root/"region.identity";
 region_options.auth_token_file.clear();region_options.register_device={};region_options.verify_device={};region_options.device_allowed={};
 region_options.upstream=auth::RelayEndpoint{server_options.relay_id,"127.0.0.1",server_options.port,"home",64,protocol::protocol_version,server_options.authority,authority_key.public_key()};
 const auto region_key=auth::DeviceIdentity::load_or_create(region_options.health_identity_file);
 transport::QuicRelayServer region(region_options,logger);region.start();
 auth::RelayEndpoint endpoint{region_options.relay_id,"127.0.0.1",region_options.port,"near",64,protocol::protocol_version,server_options.authority,region_key.public_key()};
 auto probe_at=std::chrono::steady_clock::now();
 wait([&]{if(std::chrono::steady_clock::now()<probe_at)return false;probe_at=std::chrono::steady_clock::now()+200ms;try{return transport::probe_relay(endpoint,1s,stopping).health.ready;}catch(...){return false;}});
 auto regional=options;regional.port=region_options.port;
 for(unsigned i=0;i<8;++i) {
     regional.identity_file=root/("regional-"+std::to_string(i)+".identity");regional.auth_token_file=root/("regional-"+std::to_string(i)+".credential");
     const auto device=auth::DeviceIdentity::load_or_create(regional.identity_file);
     const auto issued=transport::register_device(regional,device,authority.issue_code(10000,60),"Regional device",stopping);
     auth::AuthToken::store_credential(regional.auth_token_file,issued);
 }
 transport::QuicRelayClient regional_client(regional,logger);std::exception_ptr regional_error;
 std::jthread regional_worker([&](std::stop_token stop){try{regional_client.run(stop);}catch(...){regional_error=std::current_exception();}});
 wait([&]{return regional_client.authenticated();});
 check(regional_client.join_network(made.network_id).code==protocol::NetworkResultCode::pending_approval,"Server registration bypassed network owner approval");
 store.revoke_server_device(auth::DeviceIdentity::load_or_create(regional.identity_file).device_id());wait([&]{return !regional_client.authenticated();});
 regional_worker.request_stop();regional_client.stop();regional_worker.join();if(regional_error)std::rethrow_exception(regional_error);
 region.stop();server.stop();
 std::cout<<"Actual TLS onboarding: explicit pin, wrong pin/name rejection, one-use code, individual reconnect, network creation, regional registration, network approval separation and live revocation passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
