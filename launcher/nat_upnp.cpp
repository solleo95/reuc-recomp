#include "nat_mapping.h"
#include <charconv>
#include <stdexcept>
#include <string_view>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

namespace uc::net {
namespace {
void check(int code,const char* operation) {
    if(code!=UPNPCOMMAND_SUCCESS)throw std::runtime_error(std::string(operation)+": "+strupnperror(code)+" ("+std::to_string(code)+")");
}
class UpnpGateway final : public NatGateway {
    UPNPUrls urls_{};
    IGDdatas data_{};
public:
    UpnpGateway() {WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))throw std::runtime_error("NAT: WSAStartup failed");}
    ~UpnpGateway() override {FreeUPNPUrls(&urls_);WSACleanup();}
    RouterAddress discover() override {
        int error=0;
        std::unique_ptr<UPNPDev,decltype(&freeUPNPDevlist)> devices(upnpDiscover(1500,nullptr,nullptr,UPNP_LOCAL_PORT_ANY,0,2,&error),freeUPNPDevlist);
        if(!devices)throw std::runtime_error("UPnP router not found ("+std::to_string(error)+")");
        char local[64]{},external[64]{};
        const auto result=UPNP_GetValidIGD(devices.get(),&urls_,&data_,local,sizeof(local),external,sizeof(external));
        if(result!=UPNP_CONNECTED_IGD && result!=UPNP_PRIVATEIP_IGD)throw std::runtime_error("UPnP router is not connected");
        in_addr address{};
        if(!urls_.controlURL || InetPtonA(AF_INET,local,&address)!=1 || !address.s_addr || (ntohl(address.s_addr)>>24)==127)
            throw std::runtime_error("UPnP router returned no usable local IPv4 interface");
        check(UPNP_GetExternalIPAddress(urls_.controlURL,data_.first.servicetype,external),"external address");
        if(InetPtonA(AF_INET,external,&address)!=1)throw std::runtime_error("UPnP router returned invalid external IPv4");
        return {local,external};
    }
    std::optional<RouterRule> get(const std::string& port,const char* protocol) override {
        char client[16]{},internal[6]{},description[80]{},enabled[4]{},lease[16]{};
        const int result=UPNP_GetSpecificPortMappingEntryExt(urls_.controlURL,data_.first.servicetype,port.c_str(),protocol,nullptr,
            client,internal,description,sizeof(description),enabled,lease);
        if(result==714)return std::nullopt;
        check(result,"query port mapping");
        unsigned seconds=0;const std::string_view value=lease;
        const auto parsed=std::from_chars(value.data(),value.data()+value.size(),seconds);
        if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size())throw std::runtime_error("invalid router lease duration");
        return RouterRule{client,internal,description,std::string_view(enabled)=="1",seconds};
    }
    bool add(const std::string& port,const char* protocol,const RouterRule& rule) override {
        const auto lease=std::to_string(rule.lease);
        auto invoke=[&](const char* duration) {return UPNP_AddPortMapping(urls_.controlURL,data_.first.servicetype,port.c_str(),rule.port.c_str(),rule.client.c_str(),rule.description.c_str(),protocol,nullptr,duration);};
        int result=invoke(lease.c_str());const bool permanent=result==725;
        // shortcut: older IGDs only support permanent rules; a process crash then needs manual cleanup.
        if(permanent)result=invoke("0");
        check(result,"add port mapping");return permanent;
    }
    void remove(const std::string& port,const char* protocol) override {
        const int result=UPNP_DeletePortMapping(urls_.controlURL,data_.first.servicetype,port.c_str(),protocol,nullptr);
        if(result!=714)check(result,"delete port mapping");
    }
};
}
std::unique_ptr<NatGateway> upnp_gateway() {return std::make_unique<UpnpGateway>();}
}
