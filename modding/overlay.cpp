#include "overlay.h"

namespace uc::mods {

size_t apply_mod_overlays(DvdFs& dvd, const std::vector<const ModEntry*>& active) {
    size_t total = 0;
    for (const auto* mod : active) total += dvd.overlay(mod->root / "files");
    return total;
}

size_t load_mod_textures(gx::TextureReplacements& textures, const std::vector<const ModEntry*>& active) {
    size_t total = 0;
    for (const auto* mod : active) total = textures.load_index(mod->root / "textures", true);
    return total;
}

}  // namespace uc::mods
