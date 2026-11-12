// Which text/font pack the game runs with, chosen once at startup from the launcher language and the disc's files (game_language.cpp), and the
// guest-side index the game reads instead of its own selector 0x80242984 (hle/game_language.cpp).
#pragma once
#include <cstdint>
#include <string_view>

namespace uc {

class DvdFs;

struct GameLanguage {
    uint32_t guest_index = 2;          // the value the game's selector would leave at object+4: 2 is US English, the safe default
    std::string_view message = "us";   // Data/message/mes_<message>.txtres
    char font = 'E';                   // Data/sysres/font*/utf8_<font>.brfnt
    bool installed = true;             // false when the requested pack is incomplete and English stands in for it
};

// English for a language without a pack and for a pack with any piece missing: one language, never a mixture.
GameLanguage select_game_language(std::string_view language, const DvdFs& dvd);

// Shared and recorded runs feed both peers or a replay one guest state, so they may not follow the local launcher language.
constexpr bool english_only_run(bool netplay, bool deterministic, bool det_out, bool demo_record, bool demo_play) {
    return netplay || deterministic || det_out || demo_record || demo_play;
}

void game_language_init(uint32_t guest_index);   // verified indices only: 0, 2, 3, 4, 6, 8; anything else selects 2
uint32_t game_language_index();

}  // namespace uc
