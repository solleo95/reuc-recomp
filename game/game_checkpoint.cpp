#include "game_checkpoint.h"
#include "game_loop.h"
#include "game_input.h"
#include "game_pacing.h"
#include "prompt_hooks.h"
#include "ax_audio.h"
#include "audio_startup.h"
#include "dvdfs.h"
#include "hle/gx.h"
#include "hle/os.h"
#include "mmio.h"
#include "vi.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <cstdlib>

namespace uc {
struct GameCheckpoint {
    uint8_t* memory;
    std::shared_ptr<const RollbackMemory::Snapshot> ram;
    std::shared_ptr<const Sched::Snapshot> scheduler;
    std::shared_ptr<const OsSyncSnapshot> sync;
    std::shared_ptr<const OsAlarmSnapshot> alarms;
    std::shared_ptr<const DvdSnapshot> dvd;
    std::shared_ptr<const AudioSnapshot> audio;
    std::shared_ptr<const AudioStartupSnapshot> audio_startup;
    std::shared_ptr<const GxSnapshot> graphics;
    std::shared_ptr<const SaveFs::Snapshot> saves;
    std::shared_ptr<const PacingSnapshot> pacing;
    std::shared_ptr<const GameInputSnapshot> game_input;
    std::shared_ptr<const PromptSnapshot> prompts;
    MmioBus::RegisterSnapshot registers;
    ViSnapshot vi;
    std::vector<std::pair<NativeInput*,std::shared_ptr<const NativeInput::Snapshot>>> inputs;
    unsigned channel;
    uint64_t tick;
};
std::shared_ptr<const GameCheckpoint> game_checkpoint(RollbackMemory& memory,Sched& scheduler,SaveFs& saves,std::span<NativeInput* const> inputs,bool verify_writes) {
    const auto started=std::chrono::steady_clock::now();
    scheduler.drain_async();
    const auto drained=std::chrono::steady_clock::now();
    auto saved=std::make_shared<GameCheckpoint>();
    saved->scheduler=scheduler.checkpoint();
    saved->memory=scheduler.current()->ctx.mem;
    saved->ram=memory.capture(verify_writes);
    const auto copied=std::chrono::steady_clock::now();
    saved->sync=os_sync_checkpoint(); saved->alarms=os_alarm_checkpoint(); saved->dvd=dvd_checkpoint();
    saved->audio=audio_checkpoint(); saved->graphics=gx_checkpoint(); saved->saves=saves.checkpoint();
    saved->audio_startup=audio_startup_checkpoint();
    saved->pacing=game_pacing_checkpoint(); saved->game_input=game_input_checkpoint(); saved->vi=vi_checkpoint();
    saved->prompts=prompt_checkpoint();
    saved->registers=mmio_bus().checkpoint_registers(); saved->channel=input_hle_channel();
    for(auto* input:inputs) saved->inputs.emplace_back(input,input->checkpoint());
    saved->tick=det_clock_checkpoint();
    if(std::getenv("UC_ROLLBACK_PROFILE")) {
        static unsigned count=0; static double drain_ms=0,ram_ms=0,hle_ms=0;
        const auto ms=[](auto value) {return std::chrono::duration<double,std::milli>(value).count();};
        drain_ms+=ms(drained-started);ram_ms+=ms(copied-drained);hle_ms+=ms(std::chrono::steady_clock::now()-copied);
        if(++count==120) {std::fprintf(stderr,"[rollback-cost] 120 snapshots: drain %.2f RAM+sched %.2f HLE %.2f ms\n",drain_ms,ram_ms,hle_ms);count=0;drain_ms=ram_ms=hle_ms=0;}
    }
    return saved;
}
void game_restore(const GameCheckpoint& saved,RollbackMemory& memory,Sched& scheduler,SaveFs& saves) {
    if(saved.memory!=scheduler.current()->ctx.mem) throw std::logic_error("checkpoint belongs to another guest memory");
    scheduler.restore(*saved.scheduler);
    memory.restore(*saved.ram);
    os_sync_restore(*saved.sync); os_alarm_restore(*saved.alarms); dvd_restore(*saved.dvd);
    audio_restore(*saved.audio); gx_restore(*saved.graphics); saves.restore(*saved.saves);
    audio_startup_restore(*saved.audio_startup);
    game_pacing_restore(*saved.pacing); game_input_restore(*saved.game_input); vi_restore(saved.vi);
    prompt_restore(*saved.prompts);
    mmio_bus().restore_registers(saved.registers); input_hle_restore_channel(saved.channel);
    for(const auto& [input,state]:saved.inputs) input->restore(*state);
}
std::string game_checkpoint_difference(const GameCheckpoint& expected,const GameCheckpoint& actual) {
    if(const auto difference=RollbackMemory::difference(*expected.ram,*actual.ram);!difference.empty()) return difference;
    if(expected.tick!=actual.tick) return "virtual clock differs";
    if(Sched::checkpoint_hash(*expected.scheduler)!=Sched::checkpoint_hash(*actual.scheduler)) return "guest scheduler state differs";
    if(expected.vi.period!=actual.vi.period || expected.vi.stats.retraces!=actual.vi.stats.retraces) return "VI schedule differs";
    return {};
}
uint64_t game_checkpoint_hash(const GameCheckpoint& saved) {
    return (RollbackMemory::hash(*saved.ram)^Sched::checkpoint_hash(*saved.scheduler))*0x9E3779B97F4A7C15ull;
}
void game_checkpoint_commit_saves(const GameCheckpoint& saved,SaveFs& saves) { saves.commit(*saved.saves); }
void install_rollback_probe(Sched& scheduler,SaveFs& saves,NativeInput& input,uint64_t first_retrace,unsigned iterations) {
    if(!iterations || iterations>120) throw std::invalid_argument("rollback probe iterations must be 1..120");
    struct Probe {
        std::shared_ptr<const GameCheckpoint> start,expected;
        std::unique_ptr<RollbackMemory> memory;
        unsigned steps=0;
        bool replaying=false,done=false;
    };
    auto probe=std::make_shared<Probe>();
    scheduler.set_rollback(true);
    game_loop_set_boundary([probe,&scheduler,&saves,&input,first_retrace,iterations](ppc::Ctx& c) {
        if(probe->done || vi_stats().retraces<first_retrace) return;
        NativeInput* inputs[]={&input};
        if(!probe->start) {
            probe->memory=std::make_unique<RollbackMemory>(c.mem);
            probe->start=game_checkpoint(*probe->memory,scheduler,saves,inputs,true);
            std::fprintf(stderr,"[rollback-probe] checkpoint retrace %llu\n",(unsigned long long)vi_stats().retraces);
            return;
        }
        auto current=game_checkpoint(*probe->memory,scheduler,saves,inputs,true);
        if(++probe->steps<iterations) return;
        if(!probe->replaying) {
            probe->expected=std::move(current); probe->replaying=true; probe->steps=0;
            game_restore(*probe->start,*probe->memory,scheduler,saves); audio_set_replaying(true); gx_set_replaying(true);
            std::fprintf(stderr,"[rollback-probe] restored; replaying %u iterations\n",iterations);
            return;
        }
        audio_set_replaying(false);
        gx_set_replaying(false);
        const auto difference=game_checkpoint_difference(*probe->expected,*current);
        if(!difference.empty()) throw std::runtime_error("rollback probe mismatch: "+difference);
        std::fprintf(stderr,"[rollback-probe] PASS %u replayed iterations; RAM, thread contexts and virtual clock match\n",iterations);
        probe->done=true; probe->start.reset(); probe->expected.reset(); probe->memory.reset();
    });
}
}
