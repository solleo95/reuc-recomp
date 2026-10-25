#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace uc::net {
struct RouterAddress { std::string local,external; };
struct RouterRule {
    std::string client,port,description;
    bool enabled=false;
    unsigned lease=0;
};
// Router I/O is isolated so lifecycle tests never touch the user's router.
class NatGateway {
public:
    virtual ~NatGateway()=default;
    virtual RouterAddress discover()=0;
    virtual std::optional<RouterRule> get(const std::string& port,const char* protocol)=0;
    virtual bool add(const std::string& port,const char* protocol,const RouterRule& rule)=0; // true = permanent-only router
    virtual void remove(const std::string& port,const char* protocol)=0;
};
std::unique_ptr<NatGateway> upnp_gateway();
bool public_router_address(const std::string& address);

class PortForwarding {
public:
    PortForwarding(NatGateway& gateway,uint16_t port,std::string owner);
    PortForwarding(const PortForwarding&)=delete;
    PortForwarding& operator=(const PortForwarding&)=delete;
    ~PortForwarding();
    RouterAddress open(std::stop_token stop={});
    void renew(std::stop_token stop={});
    void close() noexcept;
    bool permanent() const {return permanent_;}
private:
    bool ours(const RouterRule& rule) const;
    void update(size_t index);
    NatGateway& gateway_;
    std::string port_,owner_;
    RouterAddress address_;
    std::array<bool,2> owned_{};
    bool permanent_=false;
};
enum class NatPhase { discovering,mapped,unavailable,stopped };
struct NatState {
    NatPhase phase=NatPhase::discovering;
    std::string external,error;
    bool public_address=false,permanent=false;
};
class NatMapping {
public:
    explicit NatMapping(uint16_t port,std::unique_ptr<NatGateway> gateway=upnp_gateway(),
                        std::chrono::milliseconds renewal=std::chrono::minutes(3));
    ~NatMapping();
    void stop();
    bool finished() const {return finished_.load();}
    NatState state() const;
private:
    mutable std::mutex mutex_;
    NatState state_;
    std::atomic<bool> finished_=false;
    std::jthread worker_;
};
}
