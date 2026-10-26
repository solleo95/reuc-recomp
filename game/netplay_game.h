#pragma once
#include "native_input.h"
#include "options.h"
#include <array>
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
uint32_t net_input_delay(uint64_t max_rtt_ns,uint32_t tick_hz);
uint64_t net_startup_rtt(std::array<uint64_t,9> samples);
uint64_t net_repair_end(uint64_t peer_ack,uint64_t latest); // Exclusive end; recent four frames are sent separately.
NetFrame predict_net_frame(const NetFrame& previous,uint64_t number);
// How many steps may run on predicted remote input. A slower peer then slows the game instead of piling up
// rollback work and history, and a correction never replays more than this many steps of its own prediction.
constexpr uint64_t net_prediction_limit=6;
bool net_must_wait(uint64_t cursor,uint64_t remote_next); // The step at cursor would be the limit+1-th predicted one.
bool same_net_input(const NetFrame&,const NetFrame&);
// Shared simulation ticks, steps per second: 30 is the original game's rate, 60 its doubling, the rest timed steps.
constexpr std::array<uint32_t,5> net_ticks{30,60,120,144,240};
// Host-side choice of the shared tick from the slowest player's smoothed wall time per simulated step.
// Called about once a second. Returns the tick to switch to, or 0 to stay: a step costing over 85% of the current frame for
// four seconds, or over 110% for two, drops one rung; a step under 65% of the next rung's frame for twenty seconds climbs one. A drop within two
// minutes of a climb doubles the wait for the next climb (up to 160 s), so a PC whose cost depends on the tick does not flap.
class NetTickPolicy {
public:
    uint32_t update(uint32_t tick,double step_cost_us);
private:
    unsigned slow_=0,fast_=0,hold_=20;
    uint64_t seconds_=0,last_up_=0;
};
class NetInputClock {
public:
    static constexpr uint64_t replay_lead=8;
    explicit NetInputClock(uint64_t delay):delay_(delay),next_(delay) {}
    // Latest slot to sample; any skipped slots retain the preceding held input. period_ns is the session tick.
    std::optional<uint64_t> sample(uint64_t cursor,uint64_t now_ns,bool replay=false,uint64_t period_ns=1000000000/120);
    uint64_t next() const { return next_; }
private:
    uint64_t delay_,next_,deadline_=0,head_=0;
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
    uint32_t tick() const;          // Simulation steps per second in force now (any thread).
    uint64_t simulation_ns() const; // Completed forward progress; replay never rewinds this telemetry.
    uint64_t consumed_sequence() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
