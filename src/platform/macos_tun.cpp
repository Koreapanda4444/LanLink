#include "tun_device.hpp"
#include "lanlink/protocol/ipv4_fanout.hpp"
#include "lanlink/protocol/network_policy.hpp"
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h>
#include <sys/socket.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstring>
#include <map>
#include <system_error>
#include <thread>
extern char** environ;
namespace lanlink::platform {
namespace {
void checked(int result,const char* operation){if(result<0)throw std::system_error(errno,std::generic_category(),operation);}
std::string ipv4(std::uint32_t value){return std::to_string(value>>24)+"."+std::to_string((value>>16)&255)+"."+std::to_string((value>>8)&255)+"."+std::to_string(value&255);}
void command(std::vector<std::string> arguments) {
    std::vector<char*> values;for(auto& value:arguments)values.push_back(value.data());values.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    auto result=posix_spawn_file_actions_init(&actions);if(result)throw std::system_error(result,std::generic_category(),"initialize network command");
    const auto out=posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0);
    const auto err=posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
    pid_t process=0;
    result=(out||err)?(out?out:err):posix_spawn(&process,values.front(),&actions,nullptr,values.data(),environ);
    posix_spawn_file_actions_destroy(&actions);
    if(result)throw std::system_error(result,std::generic_category(),"start native network command");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{5};
    for(;;){
        int status=0;const auto waited=waitpid(process,&status,WNOHANG);
        if(waited==process){if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)throw std::runtime_error("native interface or route command failed");return;}
        if(waited<0 && errno!=EINTR)throw std::system_error(errno,std::generic_category(),"wait for network command");
        if(std::chrono::steady_clock::now()>=deadline){kill(process,SIGKILL);while(waitpid(process,&status,0)<0&&errno==EINTR){}throw std::runtime_error("native network command timed out");}
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}
class MacTun final : public TunDevice {
    int fd_=-1;unsigned index_=0;std::string name_;
    std::map<protocol::NetworkId,network::VirtualNetworkAddress> addresses_;
    std::vector<network::AdapterRoute> routes_;
    void route(std::uint32_t subnet,std::uint8_t prefix,bool add){
        command({"/sbin/route","-n",add?"add":"delete","-inet","-net",ipv4(subnet)+"/"+std::to_string(prefix),"-interface",name_});
    }
    void reject_conflict(std::uint32_t subnet,std::uint8_t prefix){
        int mib[]{CTL_NET,PF_ROUTE,0,AF_INET,NET_RT_DUMP,0};std::size_t size=0;
        checked(sysctl(mib,6,nullptr,&size,nullptr,0),"size macOS route table");
        if(size>16U*1024U*1024U)throw std::runtime_error("macOS route table is too large");
        std::vector<std::byte> bytes(size);
        if(size)checked(sysctl(mib,6,bytes.data(),&size,nullptr,0),"read macOS route table");
        for(std::size_t offset=0;offset<size;){
            if(size-offset<sizeof(rt_msghdr))throw std::runtime_error("truncated macOS route table");
            const auto* message=reinterpret_cast<const rt_msghdr*>(bytes.data()+offset);
            const auto length=message->rtm_msglen;
            if(length<sizeof(rt_msghdr)||length>size-offset)throw std::runtime_error("invalid macOS route length");
            if(message->rtm_version==RTM_VERSION && message->rtm_index!=index_ && (message->rtm_flags&RTF_UP)){
                std::uint32_t destination=0,mask=0;bool ipv4_destination=false;
                auto position=offset+sizeof(rt_msghdr);
                for(unsigned field=0;field<RTAX_MAX;++field)if(message->rtm_addrs&(1U<<field)){
                    if(position+2>offset+length)throw std::runtime_error("truncated route address");
                    const auto* value=reinterpret_cast<const sockaddr*>(bytes.data()+position);
                    const auto actual=static_cast<std::size_t>(value->sa_len);
                    // Darwin route sockaddrs use 32-bit alignment on both architectures.
                    constexpr auto alignment=sizeof(std::uint32_t);
                    const auto aligned=actual?((actual+alignment-1)&~(alignment-1)):alignment;
                    if(aligned>offset+length-position)throw std::runtime_error("invalid route address size");
                    std::uint32_t address=0;
                    constexpr auto start=offsetof(sockaddr_in,sin_addr);
                    if(actual>start)std::memcpy(&address,bytes.data()+position+start,std::min<std::size_t>(4,actual-start));
                    if(field==RTAX_DST){destination=ntohl(address);ipv4_destination=value->sa_family==AF_INET;}
                    if(field==RTAX_NETMASK)mask=ntohl(address);
                    position+=aligned;
                }
                if(ipv4_destination){
                    const auto bits=static_cast<std::uint8_t>((message->rtm_flags&RTF_HOST)?32:std::countl_one(mask));
                    if(bits && protocol::subnets_overlap(subnet,prefix,destination,bits))throw std::runtime_error("utun subnet overlaps an existing IPv4 route");
                }
            }
            offset+=length;
        }
    }
public:
    MacTun(std::uint16_t mtu,const std::string& requested){
        if(!requested.empty())throw std::invalid_argument("macOS allocates utun names automatically");
        try{
            fd_=socket(PF_SYSTEM,SOCK_DGRAM,SYSPROTO_CONTROL);checked(fd_,"open macOS kernel control");
            checked(fcntl(fd_,F_SETFD,FD_CLOEXEC),"protect utun descriptor");
            ctl_info info{};std::strcpy(info.ctl_name,"com.apple.net.utun_control");
            checked(ioctl(fd_,CTLIOCGINFO,&info),"locate utun control");
            sockaddr_ctl address{};address.sc_len=sizeof(address);address.sc_family=AF_SYSTEM;address.ss_sysaddr=AF_SYS_CONTROL;address.sc_id=info.ctl_id;
            checked(connect(fd_,reinterpret_cast<sockaddr*>(&address),sizeof(address)),"allocate utun");
            std::array<char,IFNAMSIZ> name{};socklen_t length=static_cast<socklen_t>(name.size());
            checked(getsockopt(fd_,SYSPROTO_CONTROL,2,name.data(),&length),"read utun name");
            if(!std::memchr(name.data(),'\0',name.size()))throw std::runtime_error("invalid utun name");
            name_=name.data();if(!name_.starts_with("utun")||name_.size()<=4||!std::all_of(name_.begin()+4,name_.end(),[](unsigned char c){return c>='0'&&c<='9';}))throw std::runtime_error("unexpected utun name");
            index_=if_nametoindex(name_.c_str());if(!index_)throw std::runtime_error("utun interface index is missing");
            checked(fcntl(fd_,F_SETFL,O_NONBLOCK),"set nonblocking utun");
            command({"/sbin/ifconfig",name_,"mtu",std::to_string(mtu),"up"});
        }catch(...){if(fd_>=0)::close(fd_);fd_=-1;throw;}
    }
    ~MacTun() override{
        for(const auto& value:routes_)try{route(value.address,value.prefix,false);}catch(...){}
        while(!addresses_.empty()){const auto id=addresses_.begin()->first;remove(id);addresses_.erase(id);}
        if(fd_>=0)::close(fd_);
    }
    int descriptor() const noexcept override{return fd_;}
    std::string name() const override{return name_;}
    std::size_t prefix_size() const noexcept override{return 4;}
    void configure(const network::VirtualNetworkAddress& value) override{
        if(value.prefix_length<10||value.prefix_length>30||!protocol::is_ipv4_host(value.address,value.subnet,value.prefix_length)||(value.subnet&0xffc00000U)!=0x0a400000U)throw std::invalid_argument("invalid overlay utun address");
        const auto found=addresses_.find(value.network_id);if(found!=addresses_.end()&&found->second==value)return;
        for(const auto& [id,previous]:addresses_)if(id!=value.network_id&&protocol::subnets_overlap(value.subnet,value.prefix_length,previous.subnet,previous.prefix_length))throw std::runtime_error("overlapping virtual networks");
        reject_conflict(value.subnet,value.prefix_length);
        if(found!=addresses_.end())remove(value.network_id);
        const auto ip=ipv4(value.address),mask=ipv4(0xffffffffU<<(32U-value.prefix_length));
        command({"/sbin/ifconfig",name_,"inet",ip,ip,"netmask",mask,"alias"});
        try{route(value.subnet,value.prefix_length,true);addresses_.emplace(value.network_id,value);}
        catch(...){try{command({"/sbin/ifconfig",name_,"inet",ip,"-alias"});}catch(...){}throw;}
    }
    void remove(const protocol::NetworkId& id) noexcept override{
        const auto found=addresses_.find(id);if(found==addresses_.end())return;
        try{route(found->second.subnet,found->second.prefix_length,false);command({"/sbin/ifconfig",name_,"inet",ipv4(found->second.address),"-alias"});addresses_.erase(found);}catch(...){}
    }
    void routing(const network::RoutingConfiguration& configuration) override{
        if(!configuration.forwards.empty())throw std::runtime_error("Gateway forwarding requires the Windows WFP backend");
        for(const auto& value:configuration.routes){
            if(!protocol::private_routed_subnet(value.address,value.prefix)||protocol::subnets_overlap(value.address,value.prefix,0x0a400000U,10))throw std::invalid_argument("unsafe private route");
            reject_conflict(value.address,value.prefix);
        }
        for(auto it=routes_.begin();it!=routes_.end();)if(std::find(configuration.routes.begin(),configuration.routes.end(),*it)==configuration.routes.end()){route(it->address,it->prefix,false);it=routes_.erase(it);}else ++it;
        try{for(const auto& value:configuration.routes)if(std::find(routes_.begin(),routes_.end(),value)==routes_.end()){route(value.address,value.prefix,true);routes_.push_back(value);}}
        catch(...){for(const auto& value:routes_)try{route(value.address,value.prefix,false);}catch(...){}routes_.clear();throw;}
    }
};
}
std::unique_ptr<TunDevice> open_tun(std::uint16_t mtu,const std::string& name){return std::make_unique<MacTun>(mtu,name);}
}
