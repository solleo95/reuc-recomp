#pragma once
// The input domain of the mod API: what the pad reports to the game and what mods may add to it. The port calls filter_input() on every pad sample the guest
// reads (game/game_input.cpp), on the guest thread; with no mod using the API it does nothing.
#include "game_api.h"
#include <cstdint>

namespace uc::mods {

// One pad sample as the guest reads it: pointer -1..1 on both axes, held buttons (A 0x800, B 0x400, minus 0x10, plus 0x1000) and the edges since the last sample.
struct InputView {
    float x = 0, y = 0;
    uint32_t held = 0, pressed = 0, released = 0;
};

// Registers input.*: reads of the sample the game sees (after the mods), and the force / block / aim overrides, kept per mod (OwnerScope) and taken back with drop_owner.
std::shared_ptr<GameApiTicker> register_input_api(ApiRegistry&);

// Applies the mods' overrides to a sample; no-op when no input API is registered. Safe to call from the guest thread.
void filter_input(InputView& sample);

}  // namespace uc::mods
