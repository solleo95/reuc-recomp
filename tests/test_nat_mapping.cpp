#include "check.h"
#include "nat_mapping.h"
#include <map>
#include <stdexcept>
#include <functional>
#include <cstdio>
using namespace uc::net;
struct Router : NatGateway {
    std::map<std::string,RouterRule> rules;
    bool fail_udp=false,fail_discovery=false,permanent=false,lose_add_reply=false;
    unsigned adds=0,removes=0;
    std::function<void()> after_add;
    RouterAddress discover() override {if(fail_discovery)throw std::runtime_error("no router");return {"192.168.1.8","8.8.8.8"};}
    std::optional<RouterRule> get(const std::string&,const char* protocol) override {
        auto it=rules.find(protocol);return it==rules.end() ? std::nullopt : std::optional(it->second);
    }
    bool add(const std::string&,const char* protocol,const RouterRule& rule) override {
        ++adds;if(fail_udp && std::string(protocol)=="UDP")throw std::runtime_error("UDP refused");
        rules[protocol]=rule;if(permanent)rules[protocol].lease=0;
        if(after_add)after_add();
        if(lose_add_reply)throw std::runtime_error("reply lost");return permanent;
    }
    void remove(const std::string&,const char* protocol) override {++removes;rules.erase(protocol);}
};
struct RetryRouter : Router {
    std::atomic<unsigned>& calls;
    explicit RetryRouter(std::atomic<unsigned>& counter):calls(counter) {}
    bool add(const std::string& port,const char* protocol,const RouterRule& rule) override {
        if(++calls==3)throw std::runtime_error("temporary router failure");
        return Router::add(port,protocol,rule);
    }
};
template<class F> void fails(F&& f) {bool failed=false;try {f();}catch(const std::exception&) {failed=true;}CHECK(failed);}
int main(int argc,char** argv) {
    if(argc==2 && std::string(argv[1])=="--router") {
        try {
            auto router=upnp_gateway();PortForwarding mapping(*router,39887,"UC-live-probe-39887");
            const auto address=mapping.open();
            std::printf("router mapping: %s -> %s:39887 TCP+UDP, permanent=%d\n",address.external.c_str(),address.local.c_str(),mapping.permanent());
            mapping.renew();mapping.close();
            for(auto protocol:{"TCP","UDP"}) {
                const auto rule=router->get("39887",protocol);
                CHECK(!rule || rule->description!="UC-live-probe-39887");
            }
            std::puts("router probe: mapping, renewal, cleanup passed");return 0;
        } catch(const std::exception& e) {std::fprintf(stderr,"router probe unavailable: %s\n",e.what());return 3;}
    }
    CHECK(argc==1);
    {
        Router router;PortForwarding mapping(router,27888,"test-owner");
        CHECK(mapping.open().local=="192.168.1.8");CHECK(router.rules.size()==2);
        CHECK(router.rules.at("TCP").port=="27888");CHECK(router.rules.at("UDP").lease==600);
        mapping.renew();CHECK(router.adds==4);mapping.close();CHECK(router.rules.empty());
        mapping.close();CHECK(router.removes==2);
    }
    {
        Router router;router.fail_udp=true;PortForwarding mapping(router,27888,"test-owner");
        fails([&]{mapping.open();});CHECK(router.rules.empty());CHECK(router.removes==1);
    }
    {
        Router router;router.lose_add_reply=true;PortForwarding mapping(router,27888,"test-owner");
        fails([&]{mapping.open();});CHECK(router.rules.empty());
    }
    {
        Router router;router.rules["UDP"]={"192.168.1.9","27888","other app",true,0};
        PortForwarding mapping(router,27888,"test-owner");fails([&]{mapping.open();});
        CHECK(router.rules.size()==1 && router.rules.at("UDP").description=="other app");
    }
    {
        Router router;router.rules["TCP"]={"192.168.1.8","27888","manual",true,0};
        PortForwarding mapping(router,27888,"test-owner");mapping.open();mapping.renew();mapping.close();
        CHECK(router.rules.size()==1 && router.rules.at("TCP").description=="manual");CHECK(router.adds==2);
    }
    {
        Router router;PortForwarding mapping(router,27888,"test-owner");mapping.open();
        router.rules["TCP"].description="replacement";fails([&]{mapping.renew();});mapping.close();
        CHECK(router.rules.size()==1 && router.rules.at("TCP").description=="replacement");
    }
    {
        Router router;router.permanent=true;PortForwarding mapping(router,27888,"test-owner");
        mapping.open();CHECK(mapping.permanent());mapping.close();CHECK(router.rules.empty());
    }
    {
        Router router;std::stop_source stop;stop.request_stop();PortForwarding mapping(router,27888,"test-owner");
        mapping.open(stop.get_token());CHECK(router.adds==0);
    }
    {
        Router router;std::stop_source stop;router.after_add=[&]{stop.request_stop();};
        PortForwarding mapping(router,27888,"test-owner");mapping.open(stop.get_token());
        CHECK(router.adds==1 && router.rules.empty());
    }
    {
        Router router;PortForwarding mapping(router,27888,"test-owner");mapping.open();router.rules.clear();
        mapping.renew();CHECK(router.rules.size()==2);mapping.close();CHECK(router.rules.empty());
    }
    for(auto ip:{"10.0.0.1","192.168.1.1","172.16.0.1","100.64.0.1","100.127.255.255","127.0.0.1","169.254.1.1","0.0.0.0","224.0.0.1","bad"})CHECK(!public_router_address(ip));
    CHECK(public_router_address("8.8.8.8"));CHECK(public_router_address("100.128.0.1"));
    {
        auto router=std::make_unique<Router>();router->fail_discovery=true;NatMapping mapping(27888,std::move(router));
        for(int i=0;i<500 && !mapping.finished();++i)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(mapping.finished());CHECK(mapping.state().phase==NatPhase::unavailable);
    }
    {
        NatMapping mapping(27888,std::make_unique<Router>(),std::chrono::milliseconds(5));
        for(int i=0;i<500 && mapping.state().phase==NatPhase::discovering;++i)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(mapping.state().phase==NatPhase::mapped);mapping.stop();
        for(int i=0;i<500 && !mapping.finished();++i)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(mapping.finished());CHECK(mapping.state().phase==NatPhase::stopped);
    }
    {
        std::atomic<unsigned> calls=0;
        NatMapping mapping(27888,std::make_unique<RetryRouter>(calls),std::chrono::milliseconds(5));
        for(int i=0;i<500 && calls<5;++i)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(calls>=5);CHECK(!mapping.finished());
    }
}
