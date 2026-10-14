#pragma once
#include "native_input.h"
#include <array>
namespace uc {
struct InputProbePlayer {
    uint32_t address=0,mode_630=0,player_790=0,state_794=0,flags_798=0,qte_target_79C=0;
    std::array<uint32_t,6> weapon_words{}; // weapon subobject+480..494; semantic names require live proof.
    bool valid=false;
    bool operator==(const InputProbePlayer&) const=default;
};
struct InputProbeSnapshot {
    uint32_t game_mode=0,pause_state=0;
    bool game_mode_valid=false;
    uint32_t tutorial_state=0;
    bool tutorial_loaded=false;
    std::array<uint32_t,12> ui_states{};
    bool ui_valid=false,ui_enabled=false;
    uint32_t scene_address=0;
    std::array<uint32_t,5> scene_words{}; // scene+6BCC,6BD0,6BEC,6C00,6C14.
    std::array<uint32_t,6> fade_words{}; // Both action-gate fades: elapsed, reverse, duration.
    bool scene_valid=false;
    std::array<uint32_t,20> masks{};
    std::array<uint8_t,20> disabled{};
    std::array<InputProbePlayer,2> players{};
    bool bindings_valid=false;
    bool operator==(const InputProbeSnapshot&) const=default;
};
struct InputProbeStats { uint64_t samples=0,changes=0,records=0; };
InputProbeSnapshot input_probe_snapshot(const ppc::Ctx&);
// Setup before GuestRunner starts; callbacks under guest baton; stats after join.
void input_probe_init(bool);
void input_probe_sample(const ppc::Ctx&,const InputSample&);
InputProbeStats input_probe_stats();
}
