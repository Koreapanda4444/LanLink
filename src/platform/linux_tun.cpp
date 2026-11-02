#include "tun_device.hpp"
#include "lanlink/protocol/ipv4_fanout.hpp"
#include "lanlink/protocol/network_policy.hpp"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
namespace lanlink::platform {
namespace {
void checked(int value, const char* operation) {
    if (value < 0) throw std::system_error(errno, std::generic_category(), operation);
}
class LinuxTun final : public TunDevice {
    int fd_ = -1, control_ = -1, netlink_ = -1;
    unsigned index_ = 0, sequence_ = 0;
    std::string name_;
    std::map<protocol::NetworkId, network::VirtualNetworkAddress> addresses_;
    std::vector<network::AdapterRoute> routes_;
    ifreq request() const { ifreq v{}; std::memcpy(v.ifr_name, name_.c_str(), name_.size()); return v; }
    void transaction(nlmsghdr* header) {
        header->nlmsg_seq = ++sequence_;
        header->nlmsg_flags |= NLM_F_REQUEST | NLM_F_ACK;
        sockaddr_nl kernel{}; kernel.nl_family = AF_NETLINK;
        checked(static_cast<int>(sendto(netlink_, header, header->nlmsg_len, 0,
            reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel))), "send TUN netlink request");
        std::array<char, 8192> reply{};
        for (;;) {
            auto count = recv(netlink_, reply.data(), reply.size(), 0);
            if (count < 0 && errno == EINTR) continue;
            checked(static_cast<int>(count), "receive TUN netlink acknowledgement");
            for (auto* item = reinterpret_cast<nlmsghdr*>(reply.data()); NLMSG_OK(item, count); item = NLMSG_NEXT(item, count)) {
                if (item->nlmsg_seq != sequence_) continue;
                if (item->nlmsg_type != NLMSG_ERROR || item->nlmsg_len < NLMSG_LENGTH(sizeof(nlmsgerr)))
                    throw std::runtime_error("invalid TUN netlink acknowledgement");
                const auto error = reinterpret_cast<nlmsgerr*>(NLMSG_DATA(item))->error;
                if (error) throw std::system_error(-error, std::generic_category(), "configure TUN address or route");
                return;
            }
        }
    }
    template<class M, class V> void attribute(M& message, unsigned short type, const V& value) {
        auto* header = &message.header;
        const auto offset = NLMSG_ALIGN(header->nlmsg_len);
        auto* attr = reinterpret_cast<rtattr*>(reinterpret_cast<char*>(&message) + offset);
        attr->rta_type = type; attr->rta_len = RTA_LENGTH(sizeof(value));
        std::memcpy(RTA_DATA(attr), &value, sizeof(value));
        header->nlmsg_len = static_cast<unsigned>(offset + RTA_ALIGN(attr->rta_len));
    }
    void address(const network::VirtualNetworkAddress& value, bool add) {
        struct { nlmsghdr header{}; ifaddrmsg body{}; char attributes[128]{}; } m;
        m.header.nlmsg_len = NLMSG_LENGTH(sizeof(m.body));
        m.header.nlmsg_type = add ? RTM_NEWADDR : RTM_DELADDR;
        m.header.nlmsg_flags = add ? NLM_F_CREATE | NLM_F_EXCL : 0;
        m.body.ifa_family = AF_INET; m.body.ifa_prefixlen = value.prefix_length; m.body.ifa_index = index_;
        const auto ip = htonl(value.address), broadcast = htonl(value.subnet | (0xffffffffU >> value.prefix_length));
        attribute(m, IFA_LOCAL, ip); attribute(m, IFA_ADDRESS, ip); attribute(m, IFA_BROADCAST, broadcast);
        transaction(&m.header);
    }
    void route(const network::AdapterRoute& value, bool add) {
        struct { nlmsghdr header{}; rtmsg body{}; char attributes[128]{}; } m;
        m.header.nlmsg_len = NLMSG_LENGTH(sizeof(m.body)); m.header.nlmsg_type = add ? RTM_NEWROUTE : RTM_DELROUTE;
        m.header.nlmsg_flags = add ? NLM_F_CREATE | NLM_F_EXCL : 0;
        m.body.rtm_family = AF_INET; m.body.rtm_dst_len = value.prefix; m.body.rtm_table = RT_TABLE_MAIN;
        m.body.rtm_protocol = RTPROT_STATIC; m.body.rtm_scope = RT_SCOPE_LINK; m.body.rtm_type = RTN_UNICAST;
        const auto ip = htonl(value.address); attribute(m, RTA_DST, ip); attribute(m, RTA_OIF, index_);
        transaction(&m.header);
    }
    void reject_conflict(std::uint32_t subnet, std::uint8_t prefix) {
        std::ifstream input("/proc/net/route");
        if (!input) throw std::runtime_error("cannot inspect existing IPv4 routes");
        std::string line; std::getline(input, line);
        while (std::getline(input, line)) {
            std::istringstream row(line); std::string iface; std::uint32_t dst=0,gateway=0,flags=0,mask=0; unsigned refs=0,uses=0,metric=0;
            if (!(row >> iface >> std::hex >> dst >> gateway >> flags >> std::dec >> refs >> uses >> metric >> std::hex >> mask))
                throw std::runtime_error("invalid kernel route table");
            if (iface == name_ || !mask || !(flags & 1U)) continue;
            mask = ntohl(mask);
            if (protocol::subnets_overlap(subnet,prefix,ntohl(dst),static_cast<std::uint8_t>(std::countl_one(mask))))
                throw std::runtime_error("TUN subnet overlaps an existing IPv4 route");
        }
    }
public:
    LinuxTun(std::uint16_t mtu, std::string requested) {
        if (requested.empty()) requested = "lltun%d";
        if (requested.size() >= IFNAMSIZ || !std::all_of(requested.begin(),requested.end(),[](unsigned char c) {
            return std::isalnum(c) || c=='-' || c=='_' || c=='%';
        }) || (requested.find('%') != std::string::npos && requested != "lltun%d")) throw std::invalid_argument("invalid TUN name");
        try {
            fd_ = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC); checked(fd_, "open Linux TUN");
            ifreq v{}; v.ifr_flags = static_cast<short>(IFF_TUN | IFF_NO_PI | IFF_TUN_EXCL);
            std::memcpy(v.ifr_name, requested.c_str(), requested.size());
            checked(ioctl(fd_, TUNSETIFF, &v), "create exclusive Linux TUN"); name_ = v.ifr_name;
            index_ = if_nametoindex(name_.c_str()); if (!index_) throw std::runtime_error("TUN interface index is missing");
            control_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); checked(control_, "open TUN control");
            v = request(); v.ifr_mtu = mtu; checked(ioctl(control_,SIOCSIFMTU,&v), "set TUN MTU");
            v = request(); checked(ioctl(control_,SIOCGIFFLAGS,&v), "read TUN flags");
            v.ifr_flags = static_cast<short>(v.ifr_flags | IFF_UP); checked(ioctl(control_,SIOCSIFFLAGS,&v), "bring TUN up");
            netlink_ = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE); checked(netlink_, "open route netlink");
            timeval timeout{2,0}; checked(setsockopt(netlink_,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)), "set route timeout");
        } catch (...) { close(); throw; }
    }
    void close() noexcept { if(fd_>=0)::close(fd_);if(control_>=0)::close(control_);if(netlink_>=0)::close(netlink_);fd_=control_=netlink_=-1; }
    ~LinuxTun() override { close(); }
    int descriptor() const noexcept override { return fd_; }
    std::string name() const override { return name_; }
    void configure(const network::VirtualNetworkAddress& value) override {
        if (value.prefix_length < 10 || value.prefix_length > 30 ||
            !protocol::is_ipv4_host(value.address,value.subnet,value.prefix_length) ||
            (value.subnet & 0xffc00000U) != 0x0a400000U) throw std::invalid_argument("invalid overlay TUN address");
        const auto found = addresses_.find(value.network_id);
        if (found != addresses_.end() && found->second == value) return;
        for (const auto& [id, previous] : addresses_) if(id!=value.network_id && protocol::subnets_overlap(value.subnet,value.prefix_length,previous.subnet,previous.prefix_length))
            throw std::runtime_error("overlapping virtual networks");
        reject_conflict(value.subnet,value.prefix_length);
        if (found != addresses_.end()) remove(value.network_id);
        address(value,true); addresses_.emplace(value.network_id,value);
    }
    void remove(const protocol::NetworkId& id) noexcept override {
        const auto found = addresses_.find(id); if(found==addresses_.end())return;
        try { address(found->second,false); addresses_.erase(found); } catch (...) {}
    }
    void routing(const network::RoutingConfiguration& configuration) override {
        if(!configuration.forwards.empty())throw std::runtime_error("Gateway forwarding requires the Windows WFP backend");
        for(const auto& value:configuration.routes) {
            if(!protocol::private_routed_subnet(value.address,value.prefix) || protocol::subnets_overlap(value.address,value.prefix,0x0a400000U,10))
                throw std::invalid_argument("unsafe private route");
            reject_conflict(value.address,value.prefix);
        }
        for(auto it=routes_.begin();it!=routes_.end();) {
            if(std::find(configuration.routes.begin(),configuration.routes.end(),*it)==configuration.routes.end()) { route(*it,false);it=routes_.erase(it); } else ++it;
        }
        try { for(const auto& value:configuration.routes)if(std::find(routes_.begin(),routes_.end(),value)==routes_.end()) {route(value,true);routes_.push_back(value);} }
        catch(...) {for(const auto& value:routes_)try{route(value,false);}catch(...){}routes_.clear();throw;}
    }
};
}
std::unique_ptr<TunDevice> open_tun(std::uint16_t mtu, const std::string& name) { return std::make_unique<LinuxTun>(mtu,name); }
}
