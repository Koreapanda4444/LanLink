#include "lanlink/core/component.hpp"
#include "lanlink/protocol/local_control.hpp"
#include "lanlink/protocol/server_profiles.hpp"
#include "lanlink/protocol/user_access.hpp"
#include "lanlink/protocol/network_management.hpp"
#include "lanlink/protocol/join_target.hpp"
#include "lanlink/protocol/network_messages.hpp"
#include "lanlink/transport/local_pipe.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <codecvt>
#include <locale>
#include <termios.h>
#include <unistd.h>
#endif

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string utf8(const std::wstring_view text) {
#ifdef _WIN32
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length == 0) throw std::invalid_argument("invalid Unicode text");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            text.data(), static_cast<int>(text.size()), result.data(), length,
            nullptr, nullptr)) {
        throw std::invalid_argument("invalid Unicode text");
    }
    return result;
#else
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(text.data(),text.data()+text.size());
#endif
}

unsigned hex_digit(const wchar_t digit) {
    if (digit >= L'0' && digit <= L'9') return digit - L'0';
    if (digit >= L'a' && digit <= L'f') return digit - L'a' + 10;
    if (digit >= L'A' && digit <= L'F') return digit - L'A' + 10;
    throw std::invalid_argument("invalid hex identifier");
}

template <std::size_t N>
std::array<std::byte, N> parse_id(const std::wstring_view text) {
    if (text.size() != N * 2) throw std::invalid_argument("invalid identifier length");
    std::array<std::byte, N> id{};
    for (std::size_t i = 0; i < N; ++i) {
        id[i] = static_cast<std::byte>((hex_digit(text[i * 2]) << 4) |
                                        hex_digit(text[i * 2 + 1]));
    }
    return id;
}

std::string hex_id(const std::span<const std::byte> id) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(id.size() * 2);
    for (const auto byte : id) {
        const auto value = std::to_integer<unsigned>(byte);
        text += digits[value >> 4];
        text += digits[value & 15];
    }
    return text;
}

lanlink::protocol::LocalCommand command_from(const std::wstring_view action) {
    using lanlink::protocol::LocalCommand;
    if(action.starts_with(L"server-")) return LocalCommand::server_profiles;
    if (action == L"manage") return LocalCommand::network_management;
    if (action == L"routing") return LocalCommand::routing_options;
    if (action == L"invite-link" || action == L"revoke-links") return LocalCommand::user_access;
    if (action == L"password" || action == L"join-password") return LocalCommand::user_access;
    if (action == L"account") return LocalCommand::user_access;
    if (action == L"status") return LocalCommand::status;
    if (action == L"list") return LocalCommand::list;
    if (action == L"create") return LocalCommand::create;
    if (action == L"join") return LocalCommand::join;
    if (action == L"leave") return LocalCommand::leave;
    if (action == L"invite") return LocalCommand::invite;
    if (action == L"approve") return LocalCommand::approve;
    if (action == L"kick") return LocalCommand::kick;
    if (action == L"stop") return LocalCommand::stop;
    if (action == L"diagnostics") return LocalCommand::diagnostics;
    if (action == L"paths") return LocalCommand::path_diagnostics;
    if (action == L"game") return LocalCommand::game_diagnostics;
    throw std::invalid_argument("unknown local control command");
}

}

int run_cli(const int argc, wchar_t* argv[]) {
    try {
        namespace protocol = lanlink::protocol;
        const auto action = argc > 1 ? std::wstring_view{argv[1]} : L"status";
        const auto command = command_from(action);
        protocol::LocalMessage request{command, false, false, {}};
        switch (command) {
        case protocol::LocalCommand::server_profiles: {
            protocol::ServerRequest q;
            const auto value=[&](int i){if(i>=argc)throw std::invalid_argument("Missing server command argument");return utf8(argv[i]);};
            if(action==L"server-list") {if(argc!=2)throw std::invalid_argument("server-list takes no arguments");}
            else if(action==L"server-add" || action==L"server-edit") {
                const int offset=action==L"server-edit"?1:0;
                if(argc!=5+offset && argc!=6+offset)throw std::invalid_argument("server-add NAME HOST PORT [SPKI_SHA256]; server-edit ID NAME HOST PORT [SPKI_SHA256]");
                q.action=protocol::ServerAction::save;if(offset)q.profile.id=value(2);
                q.profile.name=value(2+offset);q.profile.host=value(3+offset);
                const auto port=value(4+offset);std::size_t consumed=0;const auto number=std::stoul(port,&consumed);
                if(consumed!=port.size() || number<1 || number>65535)throw std::invalid_argument("Invalid server port");q.profile.port=static_cast<std::uint16_t>(number);
                if(argc==6+offset)q.profile.public_key_pin=value(5+offset);
            } else if(action==L"server-connect" || action==L"server-delete") {
                if(argc!=3)throw std::invalid_argument("server-connect/server-delete requires a profile ID");q.profile.id=value(2);q.action=action==L"server-connect"?protocol::ServerAction::connect:protocol::ServerAction::remove;
            } else if(action==L"server-disconnect") {if(argc!=2)throw std::invalid_argument("server-disconnect takes no arguments");q.action=protocol::ServerAction::disconnect;}
            else if(action==L"server-enroll") {
                if(argc!=5)throw std::invalid_argument("server-enroll ID CODE|- DEVICE_LABEL");q.action=protocol::ServerAction::enroll;q.profile.id=value(2);q.registration_code=value(3);q.device_label=value(4);
                if(q.registration_code=="-")std::getline(std::cin,q.registration_code);
            } else if(action==L"server-accept") {
                if(argc<3)throw std::invalid_argument("server-accept URI [--confirm-switch] [--trust-pin]");q.action=protocol::ServerAction::accept_invitation;q.invitation_uri=value(2);
                for(int i=3;i<argc;++i){const auto flag=value(i);if(flag=="--confirm-switch")q.approve_server_switch=true;else if(flag=="--trust-pin")q.trust_invitation_pin=true;else throw std::invalid_argument("Unknown invitation approval flag");}
                q.expected_epoch=protocol::decode_server_state(lanlink::transport::local_pipe_request({protocol::LocalCommand::server_profiles,false,false,protocol::encode_server_request({})}).payload).connection_epoch;
            } else throw std::invalid_argument("Unknown server command");
            request.payload=protocol::encode_server_request(q);break;
        }
        case protocol::LocalCommand::status:
        case protocol::LocalCommand::stop:
        case protocol::LocalCommand::diagnostics:
        case protocol::LocalCommand::path_diagnostics:
        case protocol::LocalCommand::game_diagnostics:
            if (argc != 1 && argc != 2) {
                throw std::invalid_argument("status, stop and diagnostics take no arguments");
            }
            break;
        case protocol::LocalCommand::list:
            if (argc != 2) throw std::invalid_argument("list takes no arguments");
            request.payload = protocol::encode_network_list_request();
            break;
        case protocol::LocalCommand::network_management: {
            if(argc!=3)throw std::invalid_argument("manage requires a network ID");protocol::ManagementRequest query;query.network_id=parse_id<16>(argv[2]);request.payload=protocol::encode_management_request(query);break;
        }
        case protocol::LocalCommand::routing_options: {
            if(argc!=3)throw std::invalid_argument("routing requires status/off/routes/gateway/both");const std::wstring_view mode=argv[2];
            if(mode==L"status")break;
            if(mode!=L"off" && mode!=L"routes" && mode!=L"gateway" && mode!=L"both")throw std::invalid_argument("unknown routing option");
            request.payload={mode==L"routes" || mode==L"both"?std::byte{1}:std::byte{0},mode==L"gateway" || mode==L"both"?std::byte{1}:std::byte{0}};break;
        }
        case protocol::LocalCommand::user_access: {
            if (argc<3) throw std::invalid_argument("account requires status/create/issue/enroll/revoke");
            const std::wstring_view operation=argv[2];
            protocol::AccessRequest access;
            if(action==L"invite-link" || action==L"revoke-links") {
                if(argc!=3) throw std::invalid_argument("invite-link/revoke-links requires a network ID");
                access.network_id=parse_id<16>(argv[2]);access.action=action==L"invite-link" ? protocol::AccessAction::invite_issue : protocol::AccessAction::invite_revoke;
                access.lifetime_seconds=900;request.payload=protocol::encode_access_request(access);break;
            }
            if(action==L"password" || action==L"join-password") {
                if(argc!=3 && !(action==L"password" && argc==4 && std::wstring_view(argv[3])==L"approve")) throw std::invalid_argument("password/join-password requires a network ID; password optionally accepts approve");
                access.network_id=parse_id<16>(argv[2]);
                access.action=action==L"password" ? protocol::AccessAction::password_set : protocol::AccessAction::password_join;
                access.require_approval=argc==4;
                std::cerr << "Network password: ";
#ifdef _WIN32
                HANDLE input=GetStdHandle(STD_INPUT_HANDLE); DWORD mode=0; const bool console=GetConsoleMode(input,&mode)!=0;
                if(console) SetConsoleMode(input,mode & ~ENABLE_ECHO_INPUT);
                std::getline(std::cin,access.secret);
                if(console) { SetConsoleMode(input,mode); std::cerr << '\n'; }
#else
                termios original{};const bool console=isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO,&original)==0;
                if(console){auto hidden=original;hidden.c_lflag&=~ECHO;if(tcsetattr(STDIN_FILENO,TCSAFLUSH,&hidden)<0)throw std::runtime_error("cannot hide password input");}
                try{std::getline(std::cin,access.secret);}catch(...){if(console)tcsetattr(STDIN_FILENO,TCSAFLUSH,&original);throw;}
                if(console){tcsetattr(STDIN_FILENO,TCSAFLUSH,&original);std::cerr << '\n';}
#endif
                request.payload=protocol::encode_access_request(access); std::fill(access.secret.begin(),access.secret.end(),'\0'); break;
            }
            if (operation==L"status" && argc==3) access.action=protocol::AccessAction::account_status;
            else if (operation==L"create" && argc==4) { access.action=protocol::AccessAction::account_create; access.text=utf8(argv[3]); }
            else if (operation==L"issue" && argc==3) access.action=protocol::AccessAction::enrollment_issue;
            else if (operation==L"enroll" && argc==4) {
                access.action=protocol::AccessAction::device_enroll; access.text=utf8(argv[3]);
                std::cerr << "Enrollment code: "; std::getline(std::cin,access.secret);
            } else if (operation==L"revoke" && argc==4) { access.action=protocol::AccessAction::device_revoke; access.device_id=parse_id<32>(argv[3]); }
            else throw std::invalid_argument("invalid account arguments");
            request.payload=protocol::encode_access_request(access); break;
        }
        case protocol::LocalCommand::create:
            if (argc != 3) throw std::invalid_argument("create requires a name");
            request.payload = protocol::encode_network_create_request({utf8(argv[2])});
            break;
        case protocol::LocalCommand::join:
        case protocol::LocalCommand::leave:
            if (argc != 3) throw std::invalid_argument("join/leave requires a network ID");
            if(command==protocol::LocalCommand::join) {
                const auto target=protocol::parse_join_target(utf8(argv[2]));
                if(!target.invitation.empty()) {
                    protocol::AccessRequest access;access.action=protocol::AccessAction::invite_redeem;
                    access.network_id=target.network_id;access.secret=target.invitation;
                    request.command=protocol::LocalCommand::user_access;request.payload=protocol::encode_access_request(access);break;
                }
                request.payload=protocol::encode_network_selection_request({target.network_id});
            } else request.payload=protocol::encode_network_selection_request({parse_id<protocol::network_id_size>(argv[2])});
            break;
        case protocol::LocalCommand::invite:
        case protocol::LocalCommand::approve:
        case protocol::LocalCommand::kick:
            if (argc != 4) {
                throw std::invalid_argument("member command requires network and device IDs");
            }
            request.payload = protocol::encode_network_member_request(
                {parse_id<protocol::network_id_size>(argv[2]),
                 parse_id<protocol::network_device_id_size>(argv[3])});
            break;
        }

        std::uint64_t epoch=0;
        if(command!=protocol::LocalCommand::server_profiles && command!=protocol::LocalCommand::stop) {
            try{epoch=protocol::decode_server_state(lanlink::transport::local_pipe_request({protocol::LocalCommand::server_profiles,false,false,protocol::encode_server_request({})}).payload).connection_epoch;}catch(const std::exception&){}
        }
        auto response=lanlink::transport::local_pipe_request(epoch?protocol::scope_local_request(epoch,request):request);
        if(epoch)response=protocol::unwrap_scoped_response(command,response);
        std::cout << "LanLink " << lanlink::core::component_name(
            lanlink::core::Component::ui) << " " << lanlink::core::project_version() << '\n';
        if(command==protocol::LocalCommand::server_profiles) {
            const auto state=protocol::decode_server_state(response.payload);
            std::cout<<"selected: "<<state.selected_id<<"\nepoch: "<<state.connection_epoch<<"\nresult: "<<state.result_profile_id<<"\nnotice: "<<state.notice<<'\n';
            for(const auto& p:state.profiles)std::cout<<p.id<<" "<<p.name<<" "<<p.host<<":"<<p.port<<" registered="<<p.has_credential<<" pin="<<p.public_key_pin<<'\n';
        } else if(request.command==protocol::LocalCommand::network_management) {
            const auto result=protocol::decode_management_result(response.payload);std::cout<<protocol::network_result_code_name(result.code)<<" revision="<<result.policy.revision<<" managed="<<result.policy.managed<<" hubs="<<result.policy.hubs.size()<<" rules="<<result.policy.rules.size()<<" routes="<<result.policy.routes.size()<<'\n';
            for(const auto& e:result.audit_events)std::cout<<e.sequence<<" "<<protocol::audit_kind_name(e.kind)<<" "<<hex_id(e.actor)<<" "<<protocol::network_result_code_name(e.result)<<'\n';
            if(result.code!=protocol::NetworkResultCode::success)return EXIT_FAILURE;
        } else if(request.command==protocol::LocalCommand::routing_options) {
            if(response.payload.size()!=2)throw std::runtime_error("invalid routing response");std::cout<<"accept routes: "<<(response.payload[0]==std::byte{1})<<"\ngateway forwarding: "<<(response.payload[1]==std::byte{1})<<"\nApplies until service restart; approved private subnets only.\n";
        } else if (request.command == protocol::LocalCommand::user_access) {
            const auto result=protocol::decode_access_result(response.payload);
            std::cout << protocol::network_result_code_name(result.code) << '\n';
            if (result.code!=protocol::NetworkResultCode::success) return EXIT_FAILURE;
            if(result.action>=protocol::AccessAction::invite_issue) {
                if(!result.secret.empty()) std::cout << "Invitation link: " << result.secret << "\nInvitation code: " << result.text << '\n';
            } else {
                std::cout << "Account: " << result.text << " " << hex_id(result.account_id) << '\n';
                if (!result.secret.empty()) std::cout << "Enrollment code (5 minutes, one use): " << result.secret << '\n';
            }
            for (const auto& device:result.devices) std::cout << hex_id(device.device_id) << " " << device.label << (device.revoked ? " [revoked]" : "") << '\n';
        } else if (command == protocol::LocalCommand::status) {
            if (response.payload.size() != 8 || response.payload[3] > std::byte{2}) {
                throw std::runtime_error("invalid service status");
            }
            std::uint32_t networks = 0;
            for (std::size_t i = 0; i < 4; ++i) {
                networks |= std::to_integer<std::uint32_t>(response.payload[4 + i]) << (8 * i);
            }
            std::cout << "connected: " << (response.payload[0] != std::byte{0})
                      << "\nauthenticated: " << (response.payload[1] != std::byte{0})
                      << "\nserver configuration: " << (response.payload[3] == std::byte{1} ? "unconfigured" : response.payload[3] == std::byte{2} ? "invalid credentials" : "configured")
                      << "\ntcp fallback: " << (response.payload[2] != std::byte{0})
                      << "\nactive networks: " << networks << '\n';
        } else if (command == protocol::LocalCommand::list) {
            const auto networks = protocol::decode_network_list_result(response.payload);
            for (const auto& network : networks.networks) {
                std::cout << hex_id(network.network_id) << "  " << network.name
                          << "  members=" << network.member_count << '\n';
            }
        } else if (command == protocol::LocalCommand::stop) {
            if (!response.payload.empty()) throw std::runtime_error("invalid stop response");
            std::cout << "service stopping\n";
        } else if (command == protocol::LocalCommand::path_diagnostics) {
            const auto metrics = protocol::decode_path_diagnostics(response.payload);
            const auto& path = metrics.relay;
            std::cout << "path: " << protocol::path_kind_name(path.kind)
                      << "\nreason: " << protocol::path_reason_name(path.reason)
                      << "\nerror code: " << static_cast<unsigned>(path.error)
                      << "\nerror: " << lanlink::core::error_summary(path.error)
                      << "\nsent packets: " << path.sent_packets
                      << "\nreceived packets: " << path.received_packets
                      << "\ndropped packets: " << path.dropped_packets
                      << "\nretransmitted packets: ";
            if (path.retransmitted_packets) std::cout << *path.retransmitted_packets;
            else std::cout << "unavailable";
            std::cout << '\n';
        } else if (command == protocol::LocalCommand::game_diagnostics) {
            const auto metrics = protocol::decode_game_diagnostics(response.payload);
            std::cout << "virtual MTU: " << metrics.mtu << "\nadapter failed: " << metrics.adapter_failed
                      << "\nrejected packets: " << metrics.rejected_packets << '\n';
            for (const auto& item : metrics.networks)
                std::cout << hex_id(item.network_id) << " online peers=" << item.online_peers
                          << " discovery sent=" << item.discovery_sent << " received=" << item.discovery_received
                          << " rate limited=" << item.rate_limited << " duplicates=" << item.duplicates
                          << " reflections=" << item.reflections << '\n';
            if (metrics.truncated) std::cout << "network diagnostics truncated\n";
            std::cout << "Try the host virtual IPv4:port if LAN search fails. Check the game's inbound firewall rules.\n";
        } else if (command == protocol::LocalCommand::diagnostics) {
            const auto metrics = protocol::decode_local_diagnostics(response.payload);
            std::cout << "connected: " << metrics.connected
                      << "\nauthenticated: " << metrics.authenticated
                      << "\nserver configuration: " << (metrics.configuration == protocol::ClientConfigurationState::unconfigured ? "unconfigured" : metrics.configuration == protocol::ClientConfigurationState::invalid_credentials ? "invalid credentials" : "configured")
                      << "\ntcp fallback: " << metrics.tcp_fallback
                      << "\nactive networks: " << metrics.active_networks
                      << "\nlatency: ";
            if (metrics.rtt_us) {
                std::cout << *metrics.rtt_us / 1000.0 << " ms";
            } else {
                std::cout << "unavailable";
            }
            std::cout << "\nsent bytes: " << metrics.sent_bytes
                      << "\nreceived bytes: " << metrics.received_bytes
                      << "\nreconnect attempts: " << metrics.reconnects
                      << "\nlast error: " << metrics.last_error << '\n';
        } else {
            const auto result = protocol::decode_network_operation_result(response.payload);
            std::cout << protocol::network_result_code_name(result.code)
                      << "  " << hex_id(result.network_id) << '\n';
            if (result.code != protocol::NetworkResultCode::success &&
                result.code != protocol::NetworkResultCode::pending_approval) {
                return EXIT_FAILURE;
            }
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "lanlink-ui: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

#ifdef _WIN32
int wmain(int argc,wchar_t* argv[]){return run_cli(argc,argv);}
#else
int main(int argc,char* argv[]){
    try {
        std::vector<std::wstring> values;std::vector<wchar_t*> arguments;
        for(int i=0;i<argc;++i)values.push_back(std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(argv[i]));
        for(auto& value:values)arguments.push_back(value.data());
        return run_cli(argc,arguments.data());
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
#endif
