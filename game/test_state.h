#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace ppc { struct Ctx; }
namespace uc {
struct InputProbeSnapshot;
struct TestEnemy { uint32_t id = 0; std::array<float,3> root{}; bool head_intact = false; uint32_t actor_class = 0; float health = 0; };
struct TestFrameState {
    uint64_t frame = 0;                       // KPAD samples since start
    uint8_t context = 0;                      // InputContext value
    std::array<uint32_t,12> ui{}; bool ui_valid = false;
    std::array<uint32_t,4> menu{};            // menu page-alive words, see InputProbeSnapshot::menu_pages
    uint32_t pause = 0, scene_mode = 0, tutorial = 0; bool tutorial_loaded = false;
    bool player_valid = false; uint32_t player_address = 0, player_mode = 0, player_state = 0;
    uint32_t scene_address = 0; float damage = -1;           // scene object and its damage float at +6C00 (0 = unhurt)
    int32_t magazine = -1; float life = -1;                 // weapon words [3] and the HUD life gauge 0..1
    std::vector<TestEnemy> enemies; std::array<float,12> view{}; bool view_valid = false;   // world->view 3x4 row-major
};
struct TestAction { std::string name; bool accepted; uint64_t frame; };
void test_state_set_god(bool);                                  // --god: keep health and ammo at their maxima (cheat, marked in the report)
void test_state_apply_god(ppc::Ctx&, const InputProbeSnapshot&);  // guest thread, once per KPAD sample; no-op unless god is set
void test_state_set_skip_tutorials(bool);                       // --skip-tutorials: the original tutorial opener becomes a no-op
int test_state_skip_tutorial(ppc::Ctx&);                        // guest thread, at entry of the opener (r3 = tutorial object): its previous gate byte, or -1 when not applied
void test_state_unskip_tutorial(ppc::Ctx&, uint32_t object, int gate); // at exit of the opener: gives the gate byte back (no-op for -1)
void test_state_enable(bool);                                // before guest threads start
void test_state_publish(const InputProbeSnapshot&, uint8_t context); // guest thread, once per KPAD sample
TestFrameState test_state_latest();                             // any thread
void test_state_action(const char* name, bool accepted);        // guest thread
std::vector<TestAction> test_state_take_actions();              // main thread; bounded 256, oldest dropped
std::string test_frame_json(const TestFrameState&, uint32_t speed, double render_ms, uint64_t presents, uint64_t retraces, uint64_t dropped);
}
