#include "lanlink/core/config.hpp"
#include "lanlink/core/component.hpp"
#include "lanlink/core/logger.hpp"
#include "lanlink/relay/network_control_channel.hpp"
#include "lanlink/relay/network_service.hpp"
#include "lanlink/storage/relay_store.hpp"
#include "lanlink/transport/quic.hpp"
#include "lanlink/auth/identity.hpp"
#include "lanlink/relay/device_authority.hpp"
#include "lanlink/protocol/server_profiles.hpp"
#include "lanlink/protocol/join_target.hpp"
#include "lanlink/transport/tls.hpp"
#include "lanlink/transport/relay_probe.hpp"
#include <atomic>
#include <charconv>
#include <string_view>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int) {
    stop_requested = 1;
}

}

int main(const int argc, char* argv[]) {
    using lanlink::core::Component;

#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    try {
        if(argc==2 && std::string_view(argv[1])=="--version"){std::cout<<lanlink::core::project_version()<<'\n';return 0;}
        if(argc==8 && std::string_view(argv[1])=="--probe") {
            const auto number=[](std::string_view value,std::uint32_t maximum){
                std::uint32_t parsed=0;const auto result=std::from_chars(value.data(),value.data()+value.size(),parsed);
                if(result.ec!=std::errc{} || result.ptr!=value.data()+value.size() || !parsed || parsed>maximum)
                    throw std::invalid_argument("invalid probe port or capacity");
                return parsed;
            };
            lanlink::auth::RelayEndpoint endpoint{argv[4],argv[2],static_cast<std::uint16_t>(number(argv[3],65535))};
            endpoint.authority=argv[5];endpoint.capacity=number(argv[6],4096);
            endpoint.health_key=lanlink::auth::parse_directory_key(argv[7]);
            std::atomic_bool stopping=false;
            const auto result=lanlink::transport::probe_relay(endpoint,std::chrono::seconds(5),stopping);
            std::cout<<"ready="<<(result.health.ready?"true":"false")<<" active="<<result.health.active_connections
                     <<" capacity="<<result.health.capacity<<" rtt_us="<<result.rtt.count()<<'\n';
            return result.health.ready?0:2;
        }
        if(argc==3 && std::string_view(argv[1])=="--certificate-pin") {std::cout<<lanlink::transport::server_public_key_fingerprint(argv[2])<<'\n';return 0;}
        const auto now_ms=[] {return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();};
        if(argc>=3 && std::string_view(argv[1]).starts_with("--device-")) {
            const std::string_view action=argv[1];auto c=lanlink::core::load_config(argv[2]);lanlink::core::validate_relay_config(c);
            if(!c.relay_upstream_host.empty())throw std::invalid_argument("Manage registration on the central authority");
            lanlink::storage::RelayStore database(c.relay_database_file);auto master=lanlink::auth::AuthToken::load(c.auth_token_file);
            lanlink::relay::DeviceAuthority authority(database,master,c.relay_require_device_registration);
            if(action=="--device-invite" && (argc==5 || argc==6)) {
                lanlink::protocol::ServerInvitation invite;invite.server.name=argv[3];invite.server.host=argv[3];
                std::size_t count=0;const std::string port=argv[4];const auto number=std::stoul(port,&count);
                if(count!=port.size() || number<1 || number>65535)throw std::invalid_argument("Invalid public server port");
                invite.server.port=static_cast<std::uint16_t>(number);invite.server.public_key_pin=lanlink::transport::server_public_key_fingerprint(c.tls_certificate_file);
                if(argc==6){const auto join=lanlink::protocol::parse_join_target(argv[5]);invite.network_id=join.network_id;invite.network_invitation=join.invitation;}
                lanlink::protocol::validate_server_profile(invite.server);
                invite.registration_code=authority.issue_code(now_ms(),300);
                std::cout<<lanlink::protocol::make_server_invitation(invite)<<'\n';return 0;
            }
            if(action=="--device-code" && (argc==3 || argc==4)) {
                std::size_t consumed=0;const std::string lifetime=argc==4?argv[3]:"300";const auto duration=std::stoul(lifetime,&consumed);if(consumed!=lifetime.size())throw std::invalid_argument("Invalid registration lifetime");if(duration<1 || duration>86400)throw std::invalid_argument("Lifetime must be 1 to 86400 seconds");
                std::cout<<authority.issue_code(now_ms(),static_cast<std::uint32_t>(duration))<<'\n';return 0;
            }
            if(action=="--device-code-revoke" && argc==4){authority.revoke_code(argv[3]);std::cout<<"Registration code revoked\n";return 0;}
            if(action=="--device-list" && argc==3){for(const auto& d:database.server_devices())std::cout<<lanlink::auth::hex_encode(d.device_id)<<' '<<(d.revoked?"revoked":"approved")<<' '<<d.label<<'\n';return 0;}
            if(action=="--device-revoke" && argc==4){database.revoke_server_device(lanlink::auth::parse_directory_key(argv[3]));std::cout<<"Device access revoked\n";return 0;}
            throw std::invalid_argument("usage: --device-code CONFIG [SECONDS] | --device-code-revoke CONFIG CODE | --device-list CONFIG | --device-revoke CONFIG DEVICE_ID");
        }
        const bool health_key=argc==3 && std::string_view(argv[1])=="--health-key";
        const bool check_config=argc==3 && std::string_view(argv[1])=="--check-config";
        if(argc>2 && !health_key && !check_config)throw std::invalid_argument("usage: lanlink-relay [config-path] | --health-key config-path | --check-config config-path | --version | --probe HOST PORT ID AUTHORITY CAPACITY PUBLIC_KEY");
        auto config=argc>1?lanlink::core::load_config(std::filesystem::path{argv[(health_key || check_config)?2:1]}):lanlink::core::RuntimeConfig{};
        lanlink::core::validate_relay_config(config);
        if(check_config){std::cout<<"configuration valid\n";return 0;}
        if(health_key) {
            if(config.relay_health_identity_file.empty())throw std::runtime_error("relay health identity is not configured");
            const auto identity=lanlink::auth::DeviceIdentity::load_or_create(config.relay_health_identity_file);
            std::cout<<lanlink::auth::hex_encode(identity.public_key())<<'\n';return 0;
        }

        lanlink::core::Logger logger(config.log_directory / "relay.log",
                                     "relay",
                                     config.log_level,
                                     config.log_max_size_bytes,
                                     config.log_max_files);
        logger.info("starting");
        std::cout << "LanLink " << lanlink::core::component_name(Component::relay)
                  << " " << lanlink::core::project_version() << '\n';

        lanlink::transport::QuicServerOptions options;
        options.listen_host = config.listen_host;
        options.port = config.relay_port;
        options.certificate_file = config.tls_certificate_file;
        options.private_key_file = config.tls_private_key_file;
        options.auth_token_file = config.auth_token_file;
        options.handshake_timeout = std::chrono::milliseconds{config.quic_handshake_timeout_ms};
        options.authentication_timeout =
            std::chrono::milliseconds{config.authentication_timeout_ms};
        options.idle_timeout = std::chrono::milliseconds{config.quic_idle_timeout_ms};
        options.keep_alive_interval_ms = config.quic_keep_alive_interval_ms;
        options.max_connections = config.relay_max_connections;
        options.max_tls_handshakes = config.relay_max_tls_handshakes;
        options.send_queue.bytes = config.relay_send_queue_bytes;
        options.send_queue.frames = config.relay_send_queue_frames;
        options.control_requests_per_second = config.relay_control_requests_per_second;
        options.relay_id=config.relay_id;options.authority=config.relay_authority;
        options.health_identity_file=config.relay_health_identity_file;
        options.health_requests_per_second=config.relay_health_requests_per_second;

        std::unique_ptr<lanlink::storage::RelayStore> store;
        std::unique_ptr<lanlink::relay::NetworkService> network_service;
        std::unique_ptr<lanlink::relay::NetworkControlChannel> control_channel;
        std::unique_ptr<lanlink::auth::AuthToken> registration_master;
        std::unique_ptr<lanlink::relay::DeviceAuthority> device_authority;
        std::unique_ptr<lanlink::transport::QuicRelayServer> server;
        if(!config.relay_upstream_host.empty()) {
            lanlink::auth::RelayEndpoint home{config.relay_upstream_id,config.relay_upstream_host,config.relay_upstream_port};
            home.capacity=config.relay_upstream_capacity;home.authority=config.relay_authority;
            home.health_key=lanlink::auth::parse_directory_key(config.relay_upstream_health_public_key);
            options.upstream=std::move(home);
            server=std::make_unique<lanlink::transport::QuicRelayServer>(std::move(options),logger);
        }else {
            store=std::make_unique<lanlink::storage::RelayStore>(config.relay_database_file);
            registration_master=std::make_unique<lanlink::auth::AuthToken>(lanlink::auth::AuthToken::load(config.auth_token_file));
            device_authority=std::make_unique<lanlink::relay::DeviceAuthority>(*store,*registration_master,config.relay_require_device_registration);
            options.verify_device=[&](const auto& key,auto transcript,const auto& proof){return device_authority->verify(key,transcript,proof);};
            options.register_device=[&](auto payload){return device_authority->register_device(payload,now_ms());};
            options.device_allowed=[&](const auto& id){return device_authority->allowed(id);};
            network_service=std::make_unique<lanlink::relay::NetworkService>(*store);
            lanlink::relay::ForwardingLimits limits;
            limits.device_bytes_per_second=config.relay_device_bytes_per_second;limits.network_bytes_per_second=config.relay_network_bytes_per_second;
            limits.authority_bytes_per_second=config.relay_authority_bytes_per_second;
            limits.device_deliveries_per_second=config.relay_device_deliveries_per_second;limits.network_deliveries_per_second=config.relay_network_deliveries_per_second;
            limits.authority_deliveries_per_second=config.relay_authority_deliveries_per_second;limits.control_per_second=config.relay_control_requests_per_second;
            network_service->set_forwarding_limits(limits);
            control_channel=std::make_unique<lanlink::relay::NetworkControlChannel>(*network_service);
            server=std::make_unique<lanlink::transport::QuicRelayServer>(std::move(options),logger,*control_channel);
        }
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        server->start();

        while (stop_requested == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }

        server->stop();
        logger.info("stopped");
        logger.flush();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "lanlink-relay: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
