#include "lanlink/p2p/direct_quic.hpp"
#include "lanlink/protocol/binary.hpp"
#include "lanlink/protocol/codec.hpp"
#include "lanlink/protocol/stream_decoder.hpp"
#include "lanlink/transport/resource_limits.hpp"
#include <msquic.h>
#ifdef _WIN32
#include "../transport/windows_certificate.hpp"
#endif
#include <openssl/evp.h>
#include <openssl/pkcs12.h>
#include <openssl/x509v3.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace lanlink::p2p {
namespace {
void checked(QUIC_STATUS status,const char* operation){if(QUIC_FAILED(status))throw std::runtime_error(std::string(operation)+" failed: "+std::to_string(static_cast<unsigned long>(status)));}
QUIC_BUFFER alpn(){static std::string name="lanlink-peer/1";return {static_cast<std::uint32_t>(name.size()),reinterpret_cast<std::uint8_t*>(name.data())};}
QUIC_ADDR address(Endpoint endpoint){QUIC_ADDR a{};QuicAddrSetFamily(&a,QUIC_ADDRESS_FAMILY_INET);QuicAddrSetPort(&a,endpoint.port);a.Ipv4.sin_addr.s_addr=htonl(endpoint.ipv4);return a;}
std::string host(std::uint32_t ip){return std::to_string(ip>>24)+"."+std::to_string((ip>>16)&255)+"."+std::to_string((ip>>8)&255)+"."+std::to_string(ip&255);}
std::vector<std::byte> certificate() {
    std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key(EVP_EC_gen("prime256v1"),EVP_PKEY_free);
    std::unique_ptr<X509,decltype(&X509_free)> cert(X509_new(),X509_free);
    if(!key || !cert || X509_set_version(cert.get(),2)!=1 || ASN1_INTEGER_set(X509_get_serialNumber(cert.get()),1)!=1 ||
        !X509_gmtime_adj(X509_getm_notBefore(cert.get()),-60) || !X509_gmtime_adj(X509_getm_notAfter(cert.get()),86400) ||
        X509_set_pubkey(cert.get(),key.get())!=1)throw std::runtime_error("peer certificate creation failed");
    auto* name=X509_get_subject_name(cert.get());
    if(X509_NAME_add_entry_by_txt(name,"CN",MBSTRING_ASC,reinterpret_cast<const unsigned char*>("LanLink peer"),-1,-1,0)!=1 || X509_set_issuer_name(cert.get(),name)!=1)
        throw std::runtime_error("peer certificate name failed");
    for(auto [nid,value]:{std::pair{NID_basic_constraints,"critical,CA:FALSE"},std::pair{NID_key_usage,"critical,digitalSignature"},std::pair{NID_ext_key_usage,"serverAuth"}}){
        std::unique_ptr<X509_EXTENSION,decltype(&X509_EXTENSION_free)> extension(X509V3_EXT_conf_nid(nullptr,nullptr,nid,const_cast<char*>(value)),X509_EXTENSION_free);
        if(!extension || X509_add_ext(cert.get(),extension.get(),-1)!=1)throw std::runtime_error("peer certificate extension failed");
    }
    if(X509_sign(cert.get(),key.get(),EVP_sha256())<=0)throw std::runtime_error("peer certificate signature failed");
    std::unique_ptr<PKCS12,decltype(&PKCS12_free)> bundle(PKCS12_create("","LanLink peer",key.get(),cert.get(),nullptr,0,0,0,0,0),PKCS12_free);
    const int size=bundle?i2d_PKCS12(bundle.get(),nullptr):0;if(size<=0)throw std::runtime_error("peer certificate export failed");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));auto* destination=reinterpret_cast<unsigned char*>(bytes.data());
    if(i2d_PKCS12(bundle.get(),&destination)!=size)throw std::runtime_error("peer certificate export size failed");
    return bytes;
}

}
struct DirectQuic::Impl {
    struct Connection {
        Impl* owner;HQUIC handle=nullptr,stream=nullptr;bool outgoing=false,hello=false,proof=false,authorized=false,closed=false;
        auth::Nonce nonce=auth::random_nonce();std::optional<auth::Nonce> peer_nonce;
        std::uint16_t datagram_size=0;protocol::FrameStreamDecoder decoder;transport::SendBudget budget;
        transport::ControlRateLimit rate{32};
        Connection(Impl* o,bool out):owner(o),outgoing(out){}
    };
    struct Send {std::vector<std::byte> bytes;QUIC_BUFFER buffer;transport::SendBudget::Ticket ticket;
        Send(std::vector<std::byte> b,transport::SendBudget::Ticket t):bytes(std::move(b)),buffer{static_cast<std::uint32_t>(bytes.size()),reinterpret_cast<std::uint8_t*>(bytes.data())},ticket(std::move(t)){}
    };
    const auth::DeviceIdentity& identity;DirectQuicOptions options;transport::PacketPath::ReceiveHandler receive;
    const QUIC_API_TABLE* api=nullptr;HQUIC registration=nullptr,server_config=nullptr,client_config=nullptr,listener=nullptr;
#ifdef _WIN32
    std::unique_ptr<transport::SchannelCertificate> windows_certificate;
#endif
    mutable std::shared_mutex calls;
    mutable std::recursive_mutex mutex;std::vector<std::unique_ptr<Connection>> connections;
    bool started=false,stopping=false,lease_valid=false;std::uint16_t bound_port=0;
    std::uint64_t sent=0,received=0,dropped=0,sent_bytes=0,received_bytes=0;
    Impl(const auth::DeviceIdentity& i,DirectQuicOptions o,transport::PacketPath::ReceiveHandler r):identity(i),options(std::move(o)),receive(std::move(r)) {
        const auto& b=options.binding;
        if(i.device_id()!=b.local_device || !auth::verify_signed_device_key(options.peer_key,b.peer_device) ||
            options.peer_key.encryption_public_key!=b.peer_session || !b.key_epoch || !b.local_generation || !b.peer_generation ||
            !options.local_ipv4 || !options.peer_ipv4 || options.local_ipv4==options.peer_ipv4)throw std::invalid_argument("invalid direct peer authorization");
    }
    ~Impl(){stop();}
    void start() {
        if(started) return;
        if(!lease_valid || stopping)throw std::runtime_error("direct path has no current membership lease");
        try {
            checked(MsQuicOpen2(&api),"peer MsQuicOpen");
            QUIC_REGISTRATION_CONFIG reg{"lanlink-direct",QUIC_EXECUTION_PROFILE_LOW_LATENCY};checked(api->RegistrationOpen(&reg,&registration),"peer registration");
            QUIC_SETTINGS settings{};settings.PeerBidiStreamCount=1;settings.IsSet.PeerBidiStreamCount=TRUE;
            settings.DatagramReceiveEnabled=TRUE;settings.IsSet.DatagramReceiveEnabled=TRUE;
            settings.HandshakeIdleTimeoutMs=4000;settings.IsSet.HandshakeIdleTimeoutMs=TRUE;
            settings.IdleTimeoutMs=6000;settings.IsSet.IdleTimeoutMs=TRUE;
            settings.KeepAliveIntervalMs=2000;settings.IsSet.KeepAliveIntervalMs=TRUE;
            auto protocol=alpn();checked(api->ConfigurationOpen(registration,&protocol,1,&settings,sizeof(settings),nullptr,&server_config),"peer server configuration");
            checked(api->ConfigurationOpen(registration,&protocol,1,&settings,sizeof(settings),nullptr,&client_config),"peer client configuration");
            auto pfx=certificate();QUIC_CERTIFICATE_PKCS12 bundle{reinterpret_cast<std::uint8_t*>(pfx.data()),static_cast<std::uint32_t>(pfx.size()),""};
            QUIC_CREDENTIAL_CONFIG credentials{};
#ifdef _WIN32
            windows_certificate=std::make_unique<transport::SchannelCertificate>(pfx);
            credentials.Type=QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT;credentials.CertificateContext=windows_certificate->context();
#else
            credentials.Type=QUIC_CREDENTIAL_TYPE_CERTIFICATE_PKCS12;credentials.CertificatePkcs12=&bundle;
#endif
            checked(api->ConfigurationLoadCredential(server_config,&credentials),"peer server credentials");
            OPENSSL_cleanse(pfx.data(),pfx.size());
            credentials={};credentials.Type=QUIC_CREDENTIAL_TYPE_NONE;
            // Ephemeral TLS is followed by mandatory device signatures and packet E2E authentication.
            credentials.Flags=static_cast<QUIC_CREDENTIAL_FLAGS>(QUIC_CREDENTIAL_FLAG_CLIENT|QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION);
            checked(api->ConfigurationLoadCredential(client_config,&credentials),"peer client credentials");
            if(options.binding.local_device>options.binding.peer_device) {
                checked(api->ListenerOpen(registration,listener_callback,this,&listener),"peer listener");
                auto local=address({0,options.local_port});checked(api->ListenerStart(listener,&protocol,1,&local),"peer listener start");
                std::uint32_t size=sizeof(local);checked(api->GetParam(listener,QUIC_PARAM_LISTENER_LOCAL_ADDRESS,&size,&local),"peer listener address");
                bound_port=QuicAddrGetPort(&local);
            } else if(options.local_port) bound_port=options.local_port;
            else { UdpSocket reservation; bound_port=reservation.local_endpoint().port; }
            started=true;
        }catch(...){stop();throw;}
    }
    Connection* selected() const {
        Connection* fallback=nullptr;
        for(const auto& c:connections)if(c->authorized && !c->closed) {
            if(c->outgoing==(options.binding.local_device<options.binding.peer_device))return c.get();
            fallback=c.get();
        }
        return fallback;
    }
    void connect(Endpoint endpoint) {
        std::lock_guard lock(mutex);
        if(options.binding.local_device>options.binding.peer_device || !started || stopping || !usable_endpoint(endpoint) || connections.size()>=8 || selected())return;
        auto c=std::make_unique<Connection>(this,true);auto* raw=c.get();
        checked(api->ConnectionOpen(registration,connection_callback,raw,&c->handle),"peer connection open");connections.push_back(std::move(c));
        try {
            const std::uint8_t share=TRUE;checked(api->SetParam(raw->handle,QUIC_PARAM_CONN_SHARE_UDP_BINDING,sizeof(share),&share),"peer UDP binding");
            auto local=address({0,bound_port});checked(api->SetParam(raw->handle,QUIC_PARAM_CONN_LOCAL_ADDRESS,sizeof(local),&local),"peer local endpoint");
            auto remote=address(endpoint);checked(api->SetParam(raw->handle,QUIC_PARAM_CONN_REMOTE_ADDRESS,sizeof(remote),&remote),"peer remote endpoint");
            checked(api->ConnectionStart(raw->handle,client_config,QUIC_ADDRESS_FAMILY_INET,host(endpoint.ipv4).c_str(),endpoint.port),"peer connection start");
        }catch(...){raw->closed=true;api->ConnectionShutdown(raw->handle,QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,1);throw;}
    }
    void stop() noexcept {
        std::unique_lock lifetime(calls);
        std::vector<Connection*> handles;
        {std::lock_guard lock(mutex);if(stopping)return;stopping=true;started=false;
         for(auto& c:connections){c->authorized=false;handles.push_back(c.get());}}
        if(api){
            if(listener){api->ListenerStop(listener);api->ListenerClose(listener);listener=nullptr;}
            if(registration)api->RegistrationShutdown(registration,QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,0);
            for(auto* c:handles){if(c->stream){api->StreamClose(c->stream);c->stream=nullptr;}if(c->handle){api->ConnectionClose(c->handle);c->handle=nullptr;}}
            if(server_config){api->ConfigurationClose(server_config);server_config=nullptr;}if(client_config){api->ConfigurationClose(client_config);client_config=nullptr;}
            if(registration){api->RegistrationClose(registration);registration=nullptr;}MsQuicClose(api);api=nullptr;
        }
        connections.clear();
    }
    bool output(Connection& c,std::vector<std::byte> bytes,bool datagram,bool control) {
        auto ticket=c.budget.reserve(bytes.size(),control);if(!ticket)return false;
        auto send=std::make_unique<Send>(std::move(bytes),std::move(*ticket));auto* raw=send.release();
        const auto result=datagram?api->DatagramSend(c.handle,&raw->buffer,1,QUIC_SEND_FLAG_NONE,raw):
            api->StreamSend(c.stream,&raw->buffer,1,QUIC_SEND_FLAG_NONE,raw);
        if(QUIC_FAILED(result)){delete raw;return false;}return true;
    }
    void hello(Connection& c) {
        if(c.hello || !c.stream)return;
        c.hello=true;
        protocol::BinaryWriter w;w.append(encode_binding(options.binding));w.append(c.nonce);
        if(!output(c,protocol::encode_frame({protocol::MessageType::client_hello,0,std::move(w).finish()}),false,true))throw std::runtime_error("peer hello backpressure");
    }
    void proof(Connection& c) {
        if(c.proof || !c.peer_nonce)return;
        c.proof=true;
        const auto signature=sign_peer_binding(identity,options.binding,c.nonce,*c.peer_nonce);
        if(!output(c,protocol::encode_frame({protocol::MessageType::client_auth,0,{signature.begin(),signature.end()}}),false,true))throw std::runtime_error("peer proof backpressure");
    }
    void data(Connection& c,std::span<const std::byte> bytes) {
        if(!c.authorized || stopping || !lease_valid){++dropped;return;}
        const auto p=protocol::decode_encrypted_network_packet(bytes);
        if(p.network_id!=options.binding.network_id || p.key_epoch!=options.binding.key_epoch || p.destination_ipv4!=options.local_ipv4){++dropped;return;}
        ++received;received_bytes+=bytes.size();if(receive)receive({options.binding.peer_device,p});
    }
    void frame(Connection& c,const protocol::Frame& f) {
        if(!lease_valid || stopping)throw std::runtime_error("direct membership revoked");
        if(f.request_id)throw std::runtime_error("invalid direct request id");
        if(f.type==protocol::MessageType::network_packet_send){data(c,f.payload);return;}
        if(!c.rate.allow())throw std::runtime_error("direct control rate exceeded");
        if(f.type==protocol::MessageType::client_hello) {
            if(c.peer_nonce)throw std::runtime_error("duplicate peer hello");
            protocol::BinaryReader r(f.payload);if(decode_binding(r.bytes(168))!=options.binding.reversed())throw std::runtime_error("peer membership mismatch");
            c.peer_nonce=r.array<32>();r.finish();hello(c);proof(c);
        } else if(f.type==protocol::MessageType::client_auth) {
            protocol::BinaryReader r(f.payload);const auto signature=r.array<64>();r.finish();
            if(c.authorized || !c.peer_nonce || !c.proof || !verify_peer_binding(options.peer_key,options.binding.reversed(),*c.peer_nonce,c.nonce,signature))throw std::runtime_error("direct peer authentication failed");
            c.authorized=true;
        } else throw std::runtime_error("unexpected direct control frame");
    }
    static QUIC_STATUS QUIC_API listener_callback(HQUIC,void* context,QUIC_LISTENER_EVENT* event) {
        auto& o=*static_cast<Impl*>(context);std::lock_guard lock(o.mutex);
        if(event->Type!=QUIC_LISTENER_EVENT_NEW_CONNECTION)return QUIC_STATUS_SUCCESS;
        if(o.stopping || o.connections.size()>=8)return QUIC_STATUS_CONNECTION_REFUSED;
        try {
            auto c=std::make_unique<Connection>(&o,false);c->handle=event->NEW_CONNECTION.Connection;auto* raw=c.get();o.connections.push_back(std::move(c));
            o.api->SetCallbackHandler(raw->handle,reinterpret_cast<void*>(connection_callback),raw);
            const auto status=o.api->ConnectionSetConfiguration(raw->handle,o.server_config);
            if(QUIC_FAILED(status)){raw->closed=true;raw->handle=nullptr;}
            return status;
        }catch(...){return QUIC_STATUS_OUT_OF_MEMORY;}
    }
    static QUIC_STATUS QUIC_API connection_callback(HQUIC connection,void* context,QUIC_CONNECTION_EVENT* event) {
        auto& c=*static_cast<Connection*>(context);auto& o=*c.owner;std::lock_guard lock(o.mutex);
        try {
            switch(event->Type) {
                case QUIC_CONNECTION_EVENT_CONNECTED:
                    if(c.outgoing){checked(o.api->StreamOpen(connection,QUIC_STREAM_OPEN_FLAG_NONE,stream_callback,&c,&c.stream),"direct stream open");checked(o.api->StreamStart(c.stream,QUIC_STREAM_START_FLAG_IMMEDIATE),"direct stream start");o.hello(c);}break;
                case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
                    if(c.outgoing || c.stream){o.api->StreamClose(event->PEER_STREAM_STARTED.Stream);throw std::runtime_error("extra direct stream");}
                    c.stream=event->PEER_STREAM_STARTED.Stream;o.api->SetCallbackHandler(c.stream,reinterpret_cast<void*>(stream_callback),&c);break;
                case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:c.datagram_size=event->DATAGRAM_STATE_CHANGED.MaxSendLength;break;
                case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:o.data(c,{reinterpret_cast<const std::byte*>(event->DATAGRAM_RECEIVED.Buffer->Buffer),event->DATAGRAM_RECEIVED.Buffer->Length});break;
                case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:if(QUIC_DATAGRAM_SEND_STATE_IS_FINAL(event->DATAGRAM_SEND_STATE_CHANGED.State))delete static_cast<Send*>(event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);break;
                case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
                case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:c.authorized=false;break;
                case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:c.closed=true;c.authorized=false;break;
                default:break;
            }
        }catch(...){++o.dropped;c.authorized=false;o.api->ConnectionShutdown(connection,QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,1);}
        return QUIC_STATUS_SUCCESS;
    }
    static QUIC_STATUS QUIC_API stream_callback(HQUIC,void* context,QUIC_STREAM_EVENT* event) {
        auto& c=*static_cast<Connection*>(context);auto& o=*c.owner;std::lock_guard lock(o.mutex);
        try {
            if(event->Type==QUIC_STREAM_EVENT_SEND_COMPLETE)delete static_cast<Send*>(event->SEND_COMPLETE.ClientContext);
            else if(event->Type==QUIC_STREAM_EVENT_RECEIVE) {
                if(event->RECEIVE.TotalBufferLength>128*1024)throw std::runtime_error("direct stream receive limit");
                for(std::uint32_t i=0;i<event->RECEIVE.BufferCount;++i){const auto& b=event->RECEIVE.Buffers[i];for(const auto& f:c.decoder.push({reinterpret_cast<const std::byte*>(b.Buffer),b.Length}))o.frame(c,f);
                    if(c.decoder.buffered_size()>128*1024)throw std::runtime_error("direct stream frame limit");}
            } else if(event->Type==QUIC_STREAM_EVENT_PEER_SEND_ABORTED || event->Type==QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN)c.authorized=false;
        }catch(...){++o.dropped;c.authorized=false;o.api->ConnectionShutdown(c.handle,QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,1);}
        return QUIC_STATUS_SUCCESS;
    }
};
DirectQuic::DirectQuic(const auth::DeviceIdentity& i,DirectQuicOptions o,transport::PacketPath::ReceiveHandler r):impl_(std::make_unique<Impl>(i,std::move(o),std::move(r))){}
DirectQuic::~DirectQuic()=default;
void DirectQuic::synchronize(const session::NetworkSnapshot& snapshot) {
    auto& o=*impl_;bool revoke=false;
    {std::lock_guard lock(o.mutex);o.lease_valid=peer_binding_authorized(o.options.binding,o.options.peer_key,
        o.options.local_ipv4,o.options.peer_ipv4,snapshot);revoke=!o.lease_valid;
     if(revoke)for(auto& c:o.connections)c->authorized=false;}
    if(revoke)o.stop();
}
void DirectQuic::start(){impl_->start();}void DirectQuic::connect(Endpoint e){impl_->connect(e);}void DirectQuic::stop()noexcept{impl_->stop();}
std::uint16_t DirectQuic::port()const{std::lock_guard lock(impl_->mutex);return impl_->bound_port;}
bool DirectQuic::authenticated()const{std::lock_guard lock(impl_->mutex);return !impl_->stopping && impl_->lease_valid && impl_->selected();}
transport::PacketSendResult DirectQuic::send(const protocol::EncryptedNetworkPacket& p){
    auto& o=*impl_;std::lock_guard lock(o.mutex);auto* c=o.selected();if(!c || o.stopping || !o.lease_valid)return transport::PacketSendResult::unavailable;
    if(p.network_id!=o.options.binding.network_id || p.key_epoch!=o.options.binding.key_epoch || p.destination_ipv4!=o.options.peer_ipv4)return transport::PacketSendResult::invalid_packet;
    auto bytes=protocol::encode_encrypted_network_packet(p);const bool datagram=c->datagram_size && bytes.size()<=c->datagram_size;
    const auto size=bytes.size();if(!datagram)bytes=protocol::encode_frame({protocol::MessageType::network_packet_send,0,std::move(bytes)});
    if(!o.output(*c,std::move(bytes),datagram,false)){++o.dropped;return transport::PacketSendResult::backpressure;}
    ++o.sent;o.sent_bytes+=size;return transport::PacketSendResult::accepted;
}
transport::PathStatus DirectQuic::status()const {
    auto& o=*impl_;std::shared_lock lifetime(o.calls);transport::PathStatus s;HQUIC handle=nullptr;
    {
        std::lock_guard lock(o.mutex);auto* c=o.selected();if(c)handle=c->handle;
        s.kind=c?transport::PathKind::direct_quic:transport::PathKind::none;
        s.lifecycle=c?transport::PathLifecycle::authenticated:o.started?transport::PathLifecycle::connecting:transport::PathLifecycle::stopped;
        s.reason=c?protocol::PathReason::direct_ready:protocol::PathReason::direct_unreachable;
        s.sent_packets=o.sent;s.received_packets=o.received;s.dropped_packets=o.dropped;s.sent_bytes=o.sent_bytes;s.received_bytes=o.received_bytes;
    }
    // GetParam may run on a QUIC worker. Its callbacks must be free to acquire the state mutex.
    if(handle){QUIC_STATISTICS_V2 stats{};std::uint32_t size=sizeof(stats);if(QUIC_SUCCEEDED(o.api->GetParam(handle,QUIC_PARAM_CONN_STATISTICS_V2,&size,&stats))){s.rtt_us=stats.Rtt;s.lost_packets=stats.SendSuspectedLostPackets>=stats.SendSpuriousLostPackets?stats.SendSuspectedLostPackets-stats.SendSpuriousLostPackets:0;s.retransmittable_packets=stats.SendRetransmittablePackets;}}
    return s;
}
QualitySample DirectQuic::quality()const {
    auto& o=*impl_;std::shared_lock lifetime(o.calls);HQUIC handle=nullptr;QualitySample sample;
    {std::lock_guard lock(o.mutex);auto* c=o.selected();if(!c || o.stopping || !o.lease_valid)return sample;handle=c->handle;}
    sample.authenticated=true;QUIC_STATISTICS_V2 stats{};std::uint32_t size=sizeof(stats);
    if(QUIC_SUCCEEDED(o.api->GetParam(handle,QUIC_PARAM_CONN_STATISTICS_V2,&size,&stats))) {
        sample.rtt_us=stats.Rtt;sample.transport_sent_packets=stats.SendTotalPackets;
        sample.lost_packets=stats.SendSuspectedLostPackets>=stats.SendSpuriousLostPackets?stats.SendSuspectedLostPackets-stats.SendSpuriousLostPackets:0;
    }
    return sample;
}

}
