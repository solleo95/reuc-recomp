#pragma once
#include "native_input.h"
#include "options.h"
#include <memory>
#include <string>
namespace uc {
struct NetFrame { uint64_t number=0,checksum=0,work_ns=0,qte_epoch=0; InputFrame input; };
struct NetQteState {
    uint64_t epoch=0;
    uint32_t actor=0,mode=0,target=0;
    bool active=false;
    void update(uint32_t next_actor,uint32_t next_mode,uint32_t next_target,bool next_active,NativeInput* source=nullptr);
    void filter(NetFrame&) const;
};
std::vector<uint8_t> encode_net_frame(const NetFrame&);
NetFrame decode_net_frame(std::span<const uint8_t>);
uint64_t net_work_estimate(std::array<uint64_t,3> recent,uint64_t previous);
uint32_t net_input_delay(uint64_t max_rtt_ns);
uint64_t net_startup_rtt(std::array<uint64_t,9> samples);
uint64_t net_repair_end(uint64_t peer_ack,uint64_t latest); // Exclusive end; recent four frames are sent separately.
NetFrame predict_net_frame(const NetFrame& previous,uint64_t number);
bool same_net_input(const NetFrame&,const NetFrame&);
class NetInputClock {
public:
    explicit NetInputClock(uint64_t delay):delay_(delay),next_(delay) {}
    // Latest slot to sample; any skipped slots retain the preceding held input.
    std::optional<uint64_t> sample(uint64_t cursor,uint64_t now_ns,bool replay=false);
    uint64_t next() const { return next_; }
private:
    uint64_t delay_,next_,deadline_=0;
};
class Sched;
class SaveFs;
class NetGame {
public:
    NetGame(const Options&,NativeInput&,const std::filesystem::path& executable,const std::filesystem::path& saves);
    ~NetGame();
    void install(Sched&,SaveFs&);
    void prepare(); // Guest entry, before any save HLE call; cancellable by the UI thread.
    const std::filesystem::path& save_root() const;
    void cancel(); // Any thread; before scheduler shutdown.
    std::string error() const;
    bool waiting() const;
    uint64_t frames() const;
    uint64_t simulation_ns() const; // Completed forward progress; replay never rewinds this telemetry.
    uint64_t consumed_sequence() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
