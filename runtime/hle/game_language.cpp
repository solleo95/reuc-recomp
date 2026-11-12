// Replaces the game's text-language selector 0x80242984: it picks the pack this build selected from the launcher language instead of asking the
// console (SCGetLanguage) and remapping, where Spanish maps to the US text on this disc.
#include "game_language.h"

#include "ppc_rt.h"

namespace {

// OSGetAppGamename (0x80259320) keeps the four application-ID bytes and a NUL at r13-26624, read from the header copy or from the buffer whose
// pointer sits at r13-29256 when the slot at r13-26664 is set.
constexpr uint32_t kAppIdOverrideSlot = 26664, kAppIdSourceSlot = 29256, kAppIdNameSlot = 26624, kAppIdHeader = 0x80003180;

void refresh_app_id_name(ppc::Ctx& c) {
    const uint32_t source = ppc::rd32(c, c.r[13] - kAppIdOverrideSlot) ? ppc::rd32(c, c.r[13] - kAppIdSourceSlot) : kAppIdHeader;
    const uint32_t name = c.r[13] - kAppIdNameSlot;
    for (uint32_t i = 0; i < 4; ++i) ppc::wr8(c, name + i, ppc::rd8(c, source + i));   // byte by byte, in the original's order
    ppc::wr8(c, name + 4, 0);
}

}  // namespace

void hle_GameLanguageSelect(ppc::Ctx& c) {
    const uint32_t object = c.r[3];
    const uint32_t index = uc::game_language_index();
    // The original's English, French and Spanish branches call OSGetAppGamename first; the other branches never read the name.
    if (index == 2 || index == 4 || index == 6) refresh_app_id_name(c);
    ppc::wr32(c, object + 4, index);
}
