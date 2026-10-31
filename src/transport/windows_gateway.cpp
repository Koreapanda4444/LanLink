#include "lanlink/transport/windows_gateway.hpp"
#include <winsock2.h>
#include <windows.h>
#include <netioapi.h>
#include <fwpmu.h>
#include <rpc.h>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>

namespace lanlink::transport {
namespace {
void checked(DWORD status,const char* action){if(status!=NO_ERROR)throw std::runtime_error(std::string(action)+" failed (Windows error "+std::to_string(status)+")");}
MIB_IPINTERFACE_ROW interface_row(const NET_LUID& luid) {
    MIB_IPINTERFACE_ROW row;InitializeIpInterfaceEntry(&row);row.Family=AF_INET;row.InterfaceLuid=luid;
    checked(GetIpInterfaceEntry(&row),"GetIpInterfaceEntry(gateway)");return row;
}
GUID identifier(){GUID id{};const auto status=UuidCreate(&id);if(status!=RPC_S_OK && status!=RPC_S_UUID_LOCAL_ONLY)checked(status,"UuidCreate");return id;}
FWPM_FILTER_CONDITION0 interface_condition(const GUID& key,NET_IFINDEX index) {
    FWPM_FILTER_CONDITION0 c{};c.fieldKey=key;c.matchType=FWP_MATCH_EQUAL;c.conditionValue.type=FWP_UINT32;c.conditionValue.uint32=index;return c;
}
FWPM_FILTER_CONDITION0 address_condition(const GUID& key,FWP_V4_ADDR_AND_MASK* address) {
    FWPM_FILTER_CONDITION0 c{};c.fieldKey=key;c.matchType=FWP_MATCH_EQUAL;c.conditionValue.type=FWP_V4_ADDR_MASK;c.conditionValue.v4AddrMask=address;return c;
}
}
struct WindowsGateway::Impl {
    NET_LUID luid{};HANDLE engine=nullptr;GUID layer{};std::optional<BOOLEAN> original_forwarding;
    std::vector<UINT64> filters;std::vector<network::AdapterForward> installed;
    explicit Impl(std::uint64_t value){luid.Value=value;}
    void close() noexcept {
        // Stop forwarding before removing the deny filters.
        if(original_forwarding) {
            try{auto row=interface_row(luid);row.ForwardingEnabled=*original_forwarding;row.SitePrefixLength=0;static_cast<void>(SetIpInterfaceEntry(&row));}catch(...){}
            original_forwarding.reset();
        }
        if(engine){FwpmEngineClose0(engine);engine=nullptr;}filters.clear();installed.clear();
    }
    ~Impl(){close();}
    void open() {
        if(engine)return;
        FWPM_SESSION0 session{};session.flags=FWPM_SESSION_FLAG_DYNAMIC;session.displayData.name=const_cast<wchar_t*>(L"LanLink gateway");
        checked(FwpmEngineOpen0(nullptr,RPC_C_AUTHN_WINNT,nullptr,&session,&engine),"FwpmEngineOpen");
        layer=identifier();FWPM_SUBLAYER0 sublayer{};sublayer.subLayerKey=layer;sublayer.weight=0x7f00;
        sublayer.displayData.name=const_cast<wchar_t*>(L"LanLink isolated forwarding");
        checked(FwpmSubLayerAdd0(engine,&sublayer,nullptr),"FwpmSubLayerAdd");
    }
    UINT64 filter(std::span<FWPM_FILTER_CONDITION0> conditions,bool allow) {
        FWPM_FILTER0 f{};f.filterKey=identifier();f.layerKey=FWPM_LAYER_IPFORWARD_V4;f.subLayerKey=layer;
        f.displayData.name=const_cast<wchar_t*>(L"LanLink gateway boundary");
        UINT64 weight=allow?100:1;f.weight.type=FWP_UINT64;f.weight.uint64=&weight;
        f.action.type=allow?FWP_ACTION_PERMIT:FWP_ACTION_BLOCK;
        f.numFilterConditions=static_cast<UINT32>(conditions.size());f.filterCondition=conditions.data();
        UINT64 id=0;checked(FwpmFilterAdd0(engine,&f,nullptr,&id),"FwpmFilterAdd");return id;
    }
    void configure(std::span<const network::AdapterForward> forwards) {
        if(std::equal(forwards.begin(),forwards.end(),installed.begin(),installed.end()))return;
        if(forwards.empty()){close();return;}
        if(forwards.size()>1024)throw std::invalid_argument("gateway forwarding limit exceeded");
        NET_IFINDEX index=0;checked(ConvertInterfaceLuidToIndex(&luid,&index),"ConvertInterfaceLuidToIndex");
        struct Rule { network::AdapterForward forward;NET_IFINDEX lan; };
        std::vector<Rule> rules;
        for(const auto& rule:forwards) {
            if(!protocol::private_routed_subnet(rule.subnet,rule.prefix) || !rule.member_address)throw std::invalid_argument("unsafe gateway forwarding rule");
            if(protocol::subnets_overlap(rule.subnet,rule.prefix,0x0a400000U,10) || !protocol::subnet_contains(0x0a400000U,10,rule.member_address))throw std::invalid_argument("gateway overlay scope is invalid");
            MIB_IPFORWARD_TABLE2* table=nullptr;checked(GetIpForwardTable2(AF_INET,&table),"GetIpForwardTable2(gateway)");
            const auto release=std::unique_ptr<MIB_IPFORWARD_TABLE2,decltype(&FreeMibTable)>(table,FreeMibTable);
            MIB_IPFORWARD_ROW2 route{};bool found=false;
            for(ULONG i=0;i<table->NumEntries;++i){const auto& candidate=table->Table[i];const auto prefix=candidate.DestinationPrefix.PrefixLength;
                if(candidate.InterfaceLuid.Value==luid.Value || !prefix || prefix>rule.prefix || candidate.NextHop.Ipv4.sin_addr.s_addr!=0 ||
                    !protocol::subnet_contains(ntohl(candidate.DestinationPrefix.Prefix.Ipv4.sin_addr.s_addr),prefix,rule.subnet))continue;
                if(!found || prefix>route.DestinationPrefix.PrefixLength){route=candidate;found=true;}
            }
            if(!found)throw std::runtime_error("gateway subnet needs an existing on-link LAN route");
            if(!interface_row(route.InterfaceLuid).ForwardingEnabled)
                throw std::runtime_error("LAN interface IPv4 forwarding must be configured by the local administrator");
            rules.push_back({rule,route.InterfaceIndex});
        }
        try {
            open();checked(FwpmTransactionBegin0(engine,0),"FwpmTransactionBegin");
            std::vector<UINT64> next;
            try {
                for(const auto id:filters)checked(FwpmFilterDeleteById0(engine,id),"FwpmFilterDelete");
                auto incoming=interface_condition(FWPM_CONDITION_SOURCE_INTERFACE_INDEX,index),outgoing=interface_condition(FWPM_CONDITION_DESTINATION_INTERFACE_INDEX,index);
                next.push_back(filter(std::span(&incoming,1),false));next.push_back(filter(std::span(&outgoing,1),false));
                for(const auto& rule:rules) {
                    FWP_V4_ADDR_AND_MASK member{rule.forward.member_address,0xffffffffU},subnet{rule.forward.subnet,0xffffffffU<<(32U-rule.forward.prefix)};
                    std::array<FWPM_FILTER_CONDITION0,4> request{
                        interface_condition(FWPM_CONDITION_SOURCE_INTERFACE_INDEX,index),interface_condition(FWPM_CONDITION_DESTINATION_INTERFACE_INDEX,rule.lan),
                        address_condition(FWPM_CONDITION_IP_SOURCE_ADDRESS,&member),address_condition(FWPM_CONDITION_IP_DESTINATION_ADDRESS,&subnet)};
                    next.push_back(filter(request,true));
                    std::array<FWPM_FILTER_CONDITION0,4> reply{
                        interface_condition(FWPM_CONDITION_SOURCE_INTERFACE_INDEX,rule.lan),interface_condition(FWPM_CONDITION_DESTINATION_INTERFACE_INDEX,index),
                        address_condition(FWPM_CONDITION_IP_SOURCE_ADDRESS,&subnet),address_condition(FWPM_CONDITION_IP_DESTINATION_ADDRESS,&member)};
                    next.push_back(filter(reply,true));
                }
                checked(FwpmTransactionCommit0(engine),"FwpmTransactionCommit");
            }catch(...){FwpmTransactionAbort0(engine);throw;}
            filters=std::move(next);
            auto row=interface_row(luid);if(!original_forwarding)original_forwarding=row.ForwardingEnabled;
            row.ForwardingEnabled=TRUE;row.SitePrefixLength=0;checked(SetIpInterfaceEntry(&row),"SetIpInterfaceEntry(gateway)");
            installed.assign(forwards.begin(),forwards.end());
        }catch(...){close();throw;}
    }
};
WindowsGateway::WindowsGateway(std::uint64_t luid):impl_(std::make_unique<Impl>(luid)){}
WindowsGateway::~WindowsGateway()=default;
void WindowsGateway::configure(std::span<const network::AdapterForward> forwards){impl_->configure(forwards);}
}
