// The input domain: what the pad reports to the game, and what mods may add to it or take away. Mods act on the sample the guest is about to read.
#include "check.h"
#include "input_api.h"
#include <cmath>

using namespace uc::mods;
using nlohmann::json;

namespace {
constexpr uint32_t kA = 0x800, kB = 0x400, kMinus = 0x10, kPlus = 0x1000;

struct Fixture {
    ApiRegistry api;
    std::shared_ptr<GameApiTicker> ticker = register_input_api(api);
    json get(const std::string& path) { return api.find(path)->get(json::object()); }
    void set(const std::string& path, const json& value) { api.find(path)->set(json::object(), value); }
    bool throws(const std::function<void()>& f) { try { f(); } catch (const ApiError&) { return true; } return false; }
    static InputView sample(float x, float y, uint32_t held, uint32_t pressed = 0, uint32_t released = 0) { return {x, y, held, pressed, released}; }
};
}  // namespace

static void reads_what_the_game_sees() {
    Fixture f;
    CHECK(f.get("input.held") == 0 && f.get("input.fire_held") == false && f.get("input.aim") == json::array({0.0, 0.0}));   // before any sample
    InputView v = Fixture::sample(0.25f, -0.5f, kB | kPlus);
    filter_input(v);
    CHECK(f.get("input.held") == int(kB | kPlus) && f.get("input.fire_held") == true);
    CHECK(f.get("input.aim") == json::array({0.25, -0.5}));
    v = Fixture::sample(0, 0, kA);
    filter_input(v);
    CHECK(f.get("input.fire_held") == false && f.get("input.held") == int(kA));
}

static void force_and_block_change_the_sample() {
    Fixture f;
    f.set("input.force_held", int(kB));                      // a held trigger the player does not hold
    InputView v = Fixture::sample(0, 0, 0);
    filter_input(v);
    CHECK(v.held == kB && v.pressed == kB && v.released == 0);   // it looks like a fresh press
    v = Fixture::sample(0, 0, kB);
    filter_input(v);
    CHECK(v.held == kB && v.pressed == 0);                   // already held: no new press
    f.set("input.force_held", 0);
    f.set("input.block_held", int(kB));                      // the player's trigger does not reach the game
    v = Fixture::sample(0, 0, kB | kA, kB, 0);
    filter_input(v);
    CHECK(v.held == kA && v.pressed == 0 && v.released == kB);   // the game sees B let go
    CHECK(f.get("input.fire_held") == false);                // what the game sees, after the mods
    v = Fixture::sample(0, 0, kA | kMinus, 0, 0);
    filter_input(v);
    CHECK(v.held == (kA | kMinus));
    CHECK(f.throws([&] { f.set("input.force_held", -1); }) && f.throws([&] { f.set("input.force_held", 0x20); }) && f.throws([&] { f.set("input.block_held", "all"); }));   // only A, B, minus, plus exist
}

static void aim_override() {
    Fixture f;
    f.set("input.aim_override", json::array({0.5, -0.25}));
    InputView v = Fixture::sample(0.9f, 0.9f, 0);
    filter_input(v);
    CHECK(v.x == 0.5f && v.y == -0.25f && f.get("input.aim") == json::array({0.5, -0.25}));
    CHECK(f.get("input.aim_override") == json::array({0.5, -0.25}));
    f.set("input.aim_override", nullptr);
    v = Fixture::sample(0.9f, 0.9f, 0);
    filter_input(v);
    CHECK(v.x == 0.9f && f.get("input.aim_override").is_null());
    CHECK(f.throws([&] { f.set("input.aim_override", json::array({2.0, 0.0})); }) && f.throws([&] { f.set("input.aim_override", json::array({0.1})); }) && f.throws([&] { f.set("input.aim_override", 3); }));
}

static void pulses_survive_filtering() {
    Fixture f;
    constexpr uint32_t weapon_step=0x08000000;
    auto v=Fixture::sample(0,0,0,weapon_step|kB,kB);
    filter_input(v);
    CHECK(v.held==0 && v.pressed==(weapon_step|kB) && v.released==kB);
    f.set("input.block_held",int(kB));
    v=Fixture::sample(0,0,0,weapon_step|kB,kB);
    filter_input(v);
    CHECK(v.held==0 && v.pressed==weapon_step && v.released==kB);
}

// What a mod sets belongs to it: two mods add up, and a mod that goes takes its part back.
static void owners() {
    Fixture f;
    const std::string a = "a", b = "b";
    { OwnerScope scope(a); f.set("input.force_held", int(kB)); f.set("input.aim_override", json::array({0.1, 0.1})); }
    { OwnerScope scope(b); f.set("input.force_held", int(kA)); f.set("input.block_held", int(kMinus)); f.set("input.aim_override", json::array({0.7, 0.7})); }
    InputView v = Fixture::sample(0, 0, kMinus);
    filter_input(v);
    CHECK(v.held == (kA | kB) && v.x == 0.7f);               // masks add up, the latest aim override wins
    f.ticker->drop_owner(b);
    v = Fixture::sample(0, 0, kMinus);
    filter_input(v);
    CHECK(v.held == (kB | kMinus) && v.x == 0.1f);           // a's part stays, b's is gone (the block, the aim)
    f.ticker->drop_owner(a);
    v = Fixture::sample(0.3f, 0, 0);
    filter_input(v);
    CHECK(v.held == 0 && v.x == 0.3f);
    f.ticker->drop_owner("nobody");
}

static void no_filter_without_the_api() {
    InputView v = Fixture::sample(0.1f, 0.2f, kB);
    { Fixture f; }                                           // the API is gone again
    filter_input(v);
    CHECK(v.held == kB && v.x == 0.1f);
}

int main() {
    reads_what_the_game_sees(); force_and_block_change_the_sample(); pulses_survive_filtering(); aim_override(); owners(); no_filter_without_the_api();
    std::puts("ok");
}
