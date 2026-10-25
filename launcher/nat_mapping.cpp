#include "nat_mapping.h"
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>

namespace uc::net {
namespace {
constexpr const char* protocols[]={"TCP","UDP"};
std::string owner_id() {
    GUID guid{};if(FAILED(CoCreateGuid(&guid)))throw std::runtime_error("cannot create port mapping owner ID");
    char text[64];std::snprintf(text,sizeof(text),"UC-%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
        guid.Data1,guid.Data2,guid.Data3,guid.Data4[0],guid.Data4[1],guid.Data4[2],guid.Data4[3],guid.Data4[4],guid.Data4[5],guid.Data4[6],guid.Data4[7]);
    return text;
}
}
bool public_router_address(const std::string& address) {
    in_addr parsed{};if(InetPtonA(AF_INET,address.c_str(),&parsed)!=1)return false;
    const auto ip=ntohl(parsed.s_addr);
    return (ip>>24)!=0 && (ip>>24)!=10 && (ip>>24)!=127 && (ip>>24)<224
        && (ip&0xffc00000)!=0x64400000 && (ip&0xffff0000)!=0xa9fe0000
        && (ip&0xfff00000)!=0xac100000 && (ip&0xffff0000)!=0xc0a80000
        && (ip&0xffffff00)!=0xc0000000 && (ip&0xffffff00)!=0xc0000200
        && (ip&0xfffe0000)!=0xc6120000 && (ip&0xffffff00)!=0xc6336400 && (ip&0xffffff00)!=0xcb007100;
}
PortForwarding::PortForwarding(NatGateway& gateway,uint16_t port,std::string owner)
    :gateway_(gateway),port_(std::to_string(port)),owner_(std::move(owner)) {
    if(!port || owner_.empty())throw std::invalid_argument("invalid port mapping");
}
PortForwarding::~PortForwarding() {close();}
bool PortForwarding::ours(const RouterRule& rule) const {
    return rule.client==address_.local && rule.port==port_ && rule.description==owner_;
}
void PortForwarding::update(size_t i) {
    owned_[i]=true; // A lost Add reply can still have installed the rule; cleanup checks the unique owner.
    permanent_|=gateway_.add(port_,protocols[i],{address_.local,port_,owner_,true,600});
    const auto rule=gateway_.get(port_,protocols[i]);
    if(!rule || !ours(*rule) || !rule->enabled || (rule->lease && rule->lease<300))
        throw std::runtime_error(std::string(protocols[i])+" mapping verification failed");
    permanent_|=rule->lease==0;
}
RouterAddress PortForwarding::open(std::stop_token stop) {
    try {
        if(stop.stop_requested())return {};
        address_=gateway_.discover();
        for(size_t i=0;i<2;++i) {
            if(stop.stop_requested()) {close();return {};}
            const auto rule=gateway_.get(port_,protocols[i]);
            if(rule) {
                if(rule->client==address_.local && rule->port==port_ && rule->enabled && !rule->lease)continue;
                throw std::runtime_error(std::string(protocols[i])+" port already mapped; choose another port");
            }
            if(stop.stop_requested()) {close();return {};}
            update(i);
        }
        return address_;
    } catch(...) {close();throw;}
}
void PortForwarding::renew(std::stop_token stop) {
    for(size_t i=0;i<2 && !stop.stop_requested();++i) {
        const auto rule=gateway_.get(port_,protocols[i]);
        if(owned_[i]) {
            if(rule && !ours(*rule))throw std::runtime_error("router port mapping replaced by another owner");
            update(i);
        } else if(!rule || rule->client!=address_.local || rule->port!=port_ || !rule->enabled)
            throw std::runtime_error("existing router port mapping disappeared");
    }
}
void PortForwarding::close() noexcept {
    for(size_t i=0;i<2;++i)if(owned_[i]) {
        try {
            const auto rule=gateway_.get(port_,protocols[i]);
            if(rule && ours(*rule))gateway_.remove(port_,protocols[i]);
            owned_[i]=false;
        } catch(const std::exception& e) {std::fprintf(stderr,"[nat] cleanup %s/%s: %s\n",port_.c_str(),protocols[i],e.what());}
    }
}
NatMapping::NatMapping(uint16_t port,std::unique_ptr<NatGateway> gateway,std::chrono::milliseconds renewal) {
    if(!gateway || !port || renewal.count()<=0)throw std::invalid_argument("invalid NAT worker");
    worker_=std::jthread([this,port,renewal,gateway=std::move(gateway)](std::stop_token stop) {
        try {
            PortForwarding mapping(*gateway,port,owner_id());
            const auto address=mapping.open(stop);
            if(!stop.stop_requested()) {
                {std::lock_guard lock(mutex_);state_={NatPhase::mapped,address.external,{},public_router_address(address.external),mapping.permanent()};}
                std::condition_variable_any wake;std::mutex wait_mutex;std::unique_lock wait_lock(wait_mutex);
                auto delay=renewal;
                while(!stop.stop_requested()) {
                    wake.wait_for(wait_lock,stop,delay,[]{return false;});
                    if(!stop.stop_requested()) {
                        try {
                            mapping.renew(stop);delay=renewal;
                            std::lock_guard lock(mutex_);state_.phase=NatPhase::mapped;state_.error.clear();state_.permanent=mapping.permanent();
                        } catch(const std::exception& e) {
                            // A transient router failure must not tear down working game connections.
                            delay=std::min(renewal,std::chrono::milliseconds(15000));
                            std::lock_guard lock(mutex_);state_.phase=NatPhase::unavailable;state_.error=e.what();
                            std::fprintf(stderr,"[nat] renewal will retry: %s\n",e.what());
                        }
                    }
                }
            }
        } catch(const std::exception& e) {
            std::lock_guard lock(mutex_);state_.phase=NatPhase::unavailable;state_.error=e.what();
            std::fprintf(stderr,"[nat] %s\n",e.what());
        }
        {std::lock_guard lock(mutex_);if(stop.stop_requested())state_.phase=NatPhase::stopped;}
        finished_=true;
    });
}
NatMapping::~NatMapping() {stop();if(worker_.joinable())worker_.join();}
void NatMapping::stop() {worker_.request_stop();}
NatState NatMapping::state() const {std::lock_guard lock(mutex_);return state_;}
}
