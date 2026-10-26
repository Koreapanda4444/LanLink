#include "lanlink/session/peer_packet_path.hpp"
#include "lanlink/session/relay_packet_path.hpp"
#include "lanlink/p2p/direct_quic.hpp"
#include "lanlink/p2p/connectivity.hpp"
#include "lanlink/p2p/stun.hpp"
#include "lanlink/core/logger.hpp"
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <thread>
namespace lanlink::session {
struct PeerPacketPath::Impl {
    using Clock=std::chrono::steady_clock;
    using Key=std::pair<protocol::NetworkId,auth::DeviceId>;
    struct Receiver {
        std::mutex mutex;ReceiveHandler handler;bool active=false;
        void deliver(const protocol::ForwardedNetworkPacket& p) {
            ReceiveHandler copy;{std::lock_guard lock(mutex);if(active)copy=handler;}if(copy)copy(p);
        }
    };
    struct Peer {
        mutable std::mutex context_mutex;
        p2p::PeerBinding binding;auth::SignedDeviceKey key;std::uint32_t local_ip=0,peer_ip=0;
        std::vector<p2p::Candidate> local,remote;std::unique_ptr<p2p::UdpSocket> socket;
        std::unique_ptr<p2p::PunchSession> punch;std::unique_ptr<p2p::ConnectivityChecks> checks;
        std::atomic<std::shared_ptr<p2p::DirectQuic>> direct;
        std::atomic<protocol::PathReason> reason=protocol::PathReason::revalidating;
        Clock::time_point created=Clock::now(),expires{};bool published=false;
    };
    RelaySession& session;RelayPacketPath relay;auth::DeviceIdentity identity;core::Logger& logger;PeerPathOptions options;
    std::shared_ptr<Receiver> receiver=std::make_shared<Receiver>();
    mutable std::mutex mutex;std::map<Key,std::shared_ptr<Peer>> peers;std::jthread worker;
    std::atomic_bool enabled=true;
    Clock::time_point next_error{};
    std::uint64_t generation=0;Clock::time_point next_offer{};
    Impl(RelaySession& s,const std::filesystem::path& file,core::Logger& l,PeerPathOptions o):session(s),relay(s),identity(auth::DeviceIdentity::load_or_create(file)),logger(l),options(std::move(o)) {
        enabled.store(options.enabled);
        if(options.stun_servers.size()>2 || !options.interfaces)throw std::invalid_argument("invalid P2P configuration");
        for(auto e:options.stun_servers)if(!p2p::usable_endpoint(e,options.allow_loopback))throw std::invalid_argument("invalid STUN server");
        const std::weak_ptr<Receiver> weak=receiver;
        relay.set_receive_handler([weak](const auto& p){if(auto r=weak.lock())r->deliver(p);});
    }
    std::vector<std::shared_ptr<Peer>> entries() const {std::lock_guard lock(mutex);std::vector<std::shared_ptr<Peer>> out;for(const auto& [k,p]:peers)out.push_back(p);return out;}
    void close(const std::shared_ptr<Peer>& p) {if(auto direct=p->direct.exchange(nullptr))direct->stop();p->punch.reset();p->checks.reset();p->socket.reset();}
    void start() {
        {std::lock_guard lock(receiver->mutex);receiver->active=true;}
        relay.start();
        try{worker=std::jthread([this](std::stop_token stop){while(!stop.stop_requested()) {
            try{update();}catch(const std::exception&){if(Clock::now()>=next_error){logger.warning("P2P path update failed; relay retained");next_error=Clock::now()+std::chrono::seconds{5};}}
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }});}catch(...){relay.stop();throw;}
    }
    void stop() noexcept {
        {std::lock_guard lock(receiver->mutex);receiver->active=false;}
        worker.request_stop();if(worker.joinable())worker.join();
        auto old=entries();{std::lock_guard lock(mutex);peers.clear();}
        for(auto& p:old)close(p);
        relay.set_receive_handler({});relay.stop();
    }
    void create(const NetworkSnapshot& snapshot,const ActiveNetworkPacketState& n,const protocol::NetworkPeer& member) {
        if(!snapshot.identity || !member.signed_key)return;
        auto p=std::make_shared<Peer>();auto& b=p->binding;b.network_id=n.peers.network_id;b.key_epoch=n.key.epoch;
        b.local_device=identity.device_id();b.peer_device=member.device_id;b.local_session=snapshot.identity->signed_key.encryption_public_key;
        b.peer_session=member.signed_key->encryption_public_key;b.local_generation=++generation;
        p->key=*member.signed_key;p->local_ip=n.peers.own_address;p->peer_ip=member.ipv4_address;
        p->socket=std::make_unique<p2p::UdpSocket>();
        std::vector<p2p::ExcludedSubnet> excluded;for(const auto& network:snapshot.networks)excluded.push_back({network.peers.subnet_address,network.peers.prefix_length});
        p->local=p2p::gather_local_candidates(options.interfaces(),p->socket->local_endpoint().port,excluded,options.allow_loopback);
        if(!options.stun_servers.empty() && p->local.size()==p2p::max_candidates)p->local.pop_back();
        for(const auto server:options.stun_servers) {
            const auto candidate=p2p::discover_stun_endpoint(*p->socket,server,std::chrono::milliseconds{200});
            if(candidate && p2p::usable_endpoint(candidate->endpoint,options.allow_loopback) &&
                std::none_of(p->local.begin(),p->local.end(),[&](auto c){return c.endpoint==candidate->endpoint;}) && p->local.size()<p2p::max_candidates)p->local.push_back(*candidate);
        }
        if(p->local.empty()){p->socket.reset();p->reason=protocol::PathReason::direct_unreachable;}
        std::lock_guard lock(mutex);peers[{b.network_id,b.peer_device}]=std::move(p);
    }
    void update() {
        const auto now=Clock::now();const auto snapshot=session.snapshot();
        // Every lease is checked again before send and receive; cleanup also closes stale QUIC handles.
        for(const auto& p:entries()) {
            if(!p2p::peer_binding_authorized(p->binding,p->key,p->local_ip,p->peer_ip,snapshot) && p->binding.peer_generation) {
                {std::lock_guard lock(mutex);peers.erase({p->binding.network_id,p->binding.peer_device});}close(p);
            } else if(!snapshot.identity || snapshot.identity->signed_key.encryption_public_key!=p->binding.local_session ||
                std::none_of(snapshot.networks.begin(),snapshot.networks.end(),[&](const auto& n){return n.peers.network_id==p->binding.network_id && n.key.epoch==p->binding.key_epoch &&
                    std::any_of(n.peers.peers.begin(),n.peers.peers.end(),[&](const auto& member){return member.device_id==p->binding.peer_device && member.signed_key && *member.signed_key==p->key;});})) {
                {std::lock_guard lock(mutex);peers.erase({p->binding.network_id,p->binding.peer_device});}close(p);
            }
        }
        if(!enabled.load()) {
            auto old=entries();{std::lock_guard lock(mutex);peers.clear();}for(auto& p:old)close(p);return;
        }
        if(!snapshot.identity || !session.client().capabilities().supports(protocol::Capability::direct_quic))return;
        unsigned created=0;
        for(const auto& n:snapshot.networks)for(const auto& member:n.peers.peers) {
            if(!member.signed_key)continue;
            bool exists=false;{std::lock_guard lock(mutex);exists=peers.contains({n.peers.network_id,member.device_id}) || peers.size()>=64;}
            if(!exists && created++<2)create(snapshot,n,member);
        }
        for(auto x:session.client().take_peer_candidates()) {
            std::shared_ptr<Peer> p;{std::lock_guard lock(mutex);const auto found=peers.find({x.network_id,x.sender});if(found!=peers.end())p=found->second;}
            if(!p || x.generation<=p->binding.peer_generation || x.sender_session!=p->binding.peer_session ||
                x.recipient_session!=p->binding.local_session || x.key_epoch!=p->binding.key_epoch || p->local.empty())continue;
            if(auto direct=p->direct.exchange(nullptr))direct->stop();
            if(!p->socket)p->socket=std::make_unique<p2p::UdpSocket>(p2p::Endpoint{0,p->local.front().endpoint.port});
            {std::lock_guard lock(p->context_mutex);p->binding.peer_generation=x.generation;}
            p->remote=std::move(x.candidates);p->expires=now+std::chrono::milliseconds{x.lifetime_ms};
            p->punch.reset();p->checks.reset();p->reason=protocol::PathReason::revalidating;
        }
        for(const auto& p:entries()) {
            if(auto d=p->direct.load()){d->synchronize(snapshot);p->reason=d->authenticated()?protocol::PathReason::direct_ready:protocol::PathReason::direct_unreachable;continue;}
            if(!p->socket || p->local.empty())continue;
            if(!p->published && now>=next_offer) {
                p2p::CandidateExchange x;x.network_id=p->binding.network_id;x.sender=p->binding.local_device;x.recipient=p->binding.peer_device;
                x.sender_session=p->binding.local_session;x.recipient_session=p->binding.peer_session;x.key_epoch=p->binding.key_epoch;
                x.generation=p->binding.local_generation;x.candidates=p->local;
                p->published=session.client().publish_peer_candidates(x);next_offer=now+std::chrono::milliseconds{250};
            }
            if(!p->published || p->remote.empty())continue;
            if(now>=p->expires){p->reason=protocol::PathReason::direct_unreachable;continue;}
            if(!p->punch) {
                p->punch=std::make_unique<p2p::PunchSession>(identity,p->key,p->binding,p->remote,now);
                p->checks=std::make_unique<p2p::ConnectivityChecks>(p->binding.local_device,p->binding.peer_device,p->local,p->remote,now);
            }
            for(const auto& probe:p->punch->tick(now)) {
                if(p->socket->send(probe.destination,probe.bytes))for(const auto& local:p->local)p->checks->note_probe(local.endpoint,probe.destination);
            }
            for(unsigned i=0;i<32;++i) {
                const auto datagram=p->socket->receive(std::chrono::milliseconds{0});if(!datagram)break;
                if(const auto reply=p->punch->receive(*datagram,now))static_cast<void>(p->socket->send(reply->destination,reply->bytes));
            }
            for(auto endpoint:p->punch->verified())for(const auto& local:p->local)p->checks->result(local.endpoint,endpoint,true);
            const auto nominated=p->checks->nominate(now);
            if(!nominated) {if(p->checks->state()==p2p::ConnectivityState::failed)p->reason=protocol::PathReason::direct_unreachable;continue;}
            const auto port=p->socket->local_endpoint().port;p->socket.reset();
            const std::weak_ptr<Receiver> weak=receiver;auto* const relay_session=&session;
            const auto binding=p->binding;const auto key=p->key;const auto local=p->local_ip,peer=p->peer_ip;
            auto direct=std::make_shared<p2p::DirectQuic>(identity,p2p::DirectQuicOptions{binding,key,local,peer,port},
                [weak,relay_session,binding,key,local,peer](const auto& packet){
                    if(p2p::peer_binding_authorized(binding,key,local,peer,relay_session->snapshot()))if(auto r=weak.lock())r->deliver(packet);
                });
            direct->synchronize(snapshot);direct->start();direct->connect(nominated->remote.endpoint);p->direct.store(std::move(direct));
        }
    }
};
PeerPacketPath::PeerPacketPath(RelaySession& s,const std::filesystem::path& f,core::Logger& l,PeerPathOptions o):impl_(std::make_unique<Impl>(s,f,l,std::move(o))){}
PeerPacketPath::~PeerPacketPath(){stop();}
void PeerPacketPath::set_direct_enabled(bool enabled)noexcept{impl_->enabled.store(enabled);}
void PeerPacketPath::start(){impl_->start();}void PeerPacketPath::stop()noexcept{impl_->stop();}
void PeerPacketPath::set_receive_handler(ReceiveHandler h){std::lock_guard lock(impl_->receiver->mutex);impl_->receiver->handler=std::move(h);}
transport::PacketSendResult PeerPacketPath::send(const protocol::EncryptedNetworkPacket& packet) {
    const auto snapshot=impl_->session.snapshot();
    for(const auto& p:impl_->entries())if(p->binding.network_id==packet.network_id && p->peer_ip==packet.destination_ipv4) {
        p2p::PeerBinding binding;{std::lock_guard lock(p->context_mutex);binding=p->binding;}
        if(auto direct=p->direct.load();impl_->enabled.load() && direct && p2p::peer_binding_authorized(binding,p->key,p->local_ip,p->peer_ip,snapshot)) {
            const auto sent=direct->send(packet);if(sent==transport::PacketSendResult::accepted)return sent;
            p->reason=protocol::PathReason::direct_unreachable;
        }
        break;
    }
    return impl_->relay.send(packet);
}
transport::PathStatus PeerPacketPath::status()const{return impl_->relay.status();}
std::vector<protocol::PeerPathStatus> PeerPacketPath::peer_status()const {
    std::vector<protocol::PeerPathStatus> result;const auto relay=impl_->relay.status();
    for(const auto& p:impl_->entries()) {
        auto status=relay;if(auto direct=p->direct.load();direct && direct->authenticated())status=direct->status();
        else if(relay.kind!=transport::PathKind::relay_tls)status.reason=p->reason.load();
        result.push_back({p->binding.network_id,p->binding.peer_device,p->peer_ip,status});
    }
    return result;
}
}
