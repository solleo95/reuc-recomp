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
struct InputProbeEnemy {
    uint32_t id=0,address=0;
    std::array<float,3> root{};   // world position (units of 10 cm, y up), actor+F48
    bool head_intact=false;       // actor+1058 > 0.5; a shot-off head leaves the body to aim at
    uint32_t actor_class=0;       // class pointer at actor+10: tells zombies, leeches and bosses apart
    float health=0;               // actor+3A4 (zombie class): 10 per pistol hit, <= 0 is dead although the actor stays in the table until the wave ends
    bool operator==(const InputProbeEnemy&) const=default;
};
struct InputProbeSnapshot {
    uint32_t game_mode=0,pause_state=0;
    bool game_mode_valid=false;
    uint32_t tutorial_state=0;
    bool tutorial_loaded=false;
    std::array<uint32_t,12> ui_states{};
    // Menu page-alive words (measured, docs/spikes/G22): chapter page 804BB408, character 804BF4C0, loadout 804C0574, start dialog 804D0994.
    std::array<uint32_t,4> menu_pages{};
    bool ui_valid=false,ui_enabled=false;
    uint32_t scene_address=0;
    std::array<uint32_t,5> scene_words{}; // scene+6BCC,6BD0,6BEC,6C00,6C14.
    std::array<uint32_t,6> fade_words{}; // Both action-gate fades: elapsed, reverse, duration.
    bool scene_valid=false;
    std::array<uint32_t,20> masks{};
    std::array<uint8_t,20> disabled{};
    std::array<InputProbePlayer,2> players{};
    // Actors outside the player/pool classes (docs/spikes/G22-autoplay-memory.md), the live camera and the HUD life gauge.
    std::array<InputProbeEnemy,32> enemies{}; uint32_t enemy_count=0;
    std::array<float,12> view{}; bool view_valid=false; // world->view 3x4, row-major
    float hud_life=-1; // 0..1 in steps of 0.2, player 1
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
