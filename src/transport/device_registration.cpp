#include "lanlink/transport/device_registration.hpp"
#include "lanlink/transport/tls.hpp"
#include <atomic>
namespace lanlink::transport {
std::string register_device(const QuicClientOptions& options,const auth::DeviceIdentity& identity,
                            std::string_view code,std::string_view label,const std::atomic_bool& stopping) {
    protocol::DeviceRegistration request;request.public_key=identity.public_key();request.nonce=auth::random_nonce();request.code=code;request.label=label;
    request.signature=identity.sign(protocol::registration_transcript(request));
    auto channel=TlsChannel::connect(options.relay_host,options.port,options.handshake_timeout,stopping,options.server_public_key_pin);
    if(!channel->enqueue({protocol::MessageType::device_register_request,1,protocol::encode_device_registration(request)}))throw std::runtime_error("Cannot submit device registration");
    bool complete=false;std::string credential;
    const auto deadline=std::chrono::steady_clock::now()+options.authentication_timeout;
    channel->run([&](const protocol::Frame& frame){
        if(frame.type!=protocol::MessageType::device_register_result || frame.request_id!=1)throw std::runtime_error("Invalid registration response");
        if(frame.payload.size()!=65 || frame.payload[0]!=std::byte{1})throw std::runtime_error("Registration code is invalid, expired, used, or revoked");
        credential.assign(reinterpret_cast<const char*>(frame.payload.data()+1),64);
        if(credential.find_first_not_of("0123456789abcdef")!=credential.npos)throw std::runtime_error("Invalid issued device credential");
        complete=true;
    },[&]{return !complete && !stopping && std::chrono::steady_clock::now()<deadline;});
    channel->stop();
    if(!complete)throw std::runtime_error("Device registration timed out");
    return credential;
}
}
