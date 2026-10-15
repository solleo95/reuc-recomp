#include "test_state.h"
#include "input_probe.h"
#include "ppc_rt.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
namespace uc {
namespace {
std::atomic<bool> enabled{false}, god{false}, skip_tutorials{false};
std::mutex lock;
TestFrameState latest;
std::deque<TestAction> actions;
}
void test_state_enable(bool on) { enabled = on; }
void test_state_set_god(bool on) { god = on; }
// Addresses of RBUE08 (docs/spikes/G22-autoplay-memory.md): damage float at scene+6C00 (0 = unhurt, 50 = dead), magazine at
// player+4D4, grenade count at 8056F338. Dolphin Gecko codes (memory_cheat_codes.txt) write the same words.
void test_state_apply_god(ppc::Ctx& c, const InputProbeSnapshot& s) {
    if (!god.load(std::memory_order_relaxed)) return;
    if (s.scene_valid && s.scene_address) ppc::wr32(c, s.scene_address + 0x6C00, 0);
    if (s.players[0].valid) { if (ppc::rd32(c, s.players[0].address + 0x4D4) < 99) ppc::wr32(c, s.players[0].address + 0x4D4, 99); }
    ppc::wr32(c, 0x8056F338, 9);
}
void test_state_set_skip_tutorials(bool on) { skip_tutorials = on; }
// The opener 8024F280 returns 0 at once when the object's byte +4 is 0; the scenario script at 801092A0 then polls the busy getter
// 8024F558 (state +48 != 00100000), so the state is set to "closed" to let it continue. The gate byte is only zero during the call.
int test_state_skip_tutorial(ppc::Ctx& c) {
    if (!skip_tutorials.load(std::memory_order_relaxed) || !c.r[3]) return -1;
    const int gate = ppc::rd8(c, c.r[3] + 4);
    ppc::wr8(c, c.r[3] + 4, 0);
    ppc::wr32(c, c.r[3] + 0x48, 0x00100000);
    return gate;
}
void test_state_unskip_tutorial(ppc::Ctx& c, uint32_t object, int gate) {
    if (gate >= 0) ppc::wr8(c, object + 4, uint8_t(gate));
}
void test_state_publish(const InputProbeSnapshot& s, uint8_t context) {
    if (!enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard g(lock);
    ++latest.frame; latest.context = context; latest.ui = s.ui_states; latest.ui_valid = s.ui_valid; latest.menu = s.menu_pages;
    latest.pause = s.pause_state; latest.scene_mode = s.game_mode; latest.tutorial = s.tutorial_state; latest.tutorial_loaded = s.tutorial_loaded;
    latest.scene_address = s.scene_address; latest.damage = s.scene_valid ? std::bit_cast<float>(s.scene_words[3]) : -1.f;
    latest.magazine = int32_t(s.players[0].weapon_words[3]); latest.life = s.hud_life; latest.view = s.view; latest.view_valid = s.view_valid;
    latest.enemies.clear(); for (uint32_t i = 0; i < s.enemy_count; ++i) latest.enemies.push_back({s.enemies[i].id, s.enemies[i].root, s.enemies[i].head_intact, s.enemies[i].actor_class, s.enemies[i].health});
    latest.player_valid = s.players[0].valid; latest.player_address = s.players[0].address; latest.player_mode = s.players[0].mode_630; latest.player_state = s.players[0].state_794;
}
TestFrameState test_state_latest() { std::lock_guard g(lock); return latest; }
void test_state_action(const char* name, bool accepted) {
    if (!enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard g(lock);
    if (actions.size() == 256) actions.pop_front();
    actions.push_back({name, accepted, latest.frame});
}
std::vector<TestAction> test_state_take_actions() { std::lock_guard g(lock); std::vector<TestAction> out(actions.begin(), actions.end()); actions.clear(); return out; }
std::string test_frame_json(const TestFrameState& f, uint32_t speed, double render_ms, uint64_t presents, uint64_t retraces, uint64_t dropped) {
    static const char* names[] = {"inactive", "menu", "gameplay", "qte"};
    std::string ui = "[";
    for (size_t i = 0; i < f.ui.size(); ++i) ui += (i ? "," : "") + std::to_string(int32_t(f.ui[i]));   // signed: -1 = inactive slot
    ui += "]";
    char buffer[1024];
    std::snprintf(buffer, sizeof buffer,
        R"({"type":"frame","n":%llu,"speed":%u,"context":"%s","ui":%s,"menu":[%u,%u,%u,%u],"ui_valid":%s,"pause":%u,"scene_mode":%u,"tutorial":%u,"tutorial_loaded":%s,"player":{"valid":%s,"address":%u,"mode":%u,"state":%u,"magazine":%d,"life":%.2f,"damage":%.3f,"scene":%u},"render_ms":%.3f,"presents":%llu,"retraces":%llu,"dropped":%llu)",
        (unsigned long long)f.frame, speed, names[f.context & 3], ui.c_str(), f.menu[0], f.menu[1], f.menu[2], f.menu[3], f.ui_valid ? "true" : "false", f.pause, f.scene_mode, f.tutorial,
        f.tutorial_loaded ? "true" : "false", f.player_valid ? "true" : "false", f.player_address, f.player_mode, f.player_state, f.magazine, f.life, f.damage, f.scene_address, render_ms,
        (unsigned long long)presents, (unsigned long long)retraces, (unsigned long long)dropped);
    std::string out = buffer;
    out += ",\"view\":";
    if (!f.view_valid) out += "null";
    else {
        out += "[";
        for (size_t i = 0; i < f.view.size(); ++i) { char number[32]; std::snprintf(number, sizeof number, "%s%.9g", i ? "," : "", f.view[i]); out += number; }
        out += "]";
    }
    out += ",\"enemies\":[";
    for (size_t i = 0; i < f.enemies.size(); ++i) {
        const auto& e = f.enemies[i]; char text[256];
        std::snprintf(text, sizeof text, R"(%s{"id":%u,"cls":%u,"root":[%.3f,%.3f,%.3f],"head":%s,"hp":%.1f})", i ? "," : "", e.id, e.actor_class, e.root[0], e.root[1], e.root[2], e.head_intact ? "true" : "false", std::isfinite(e.health) ? e.health : 0.f);
        out += text;
    }
    return out + "]}";
}
}
