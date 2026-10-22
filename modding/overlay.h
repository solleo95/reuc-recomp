#pragma once
#include "dvdfs.h"
#include "registry.h"
#include "texture_replacement.h"
#include <vector>

namespace uc::mods {

// Applies <mod>/files to the disc tree, mods in load order (the last one wins). Returns files replaced + added in total.
size_t apply_mod_overlays(DvdFs& dvd, const std::vector<const ModEntry*>& active);

// Indexes <mod>/textures after whatever the table already holds, mods in load order (the last one wins). Returns the table size.
size_t load_mod_textures(gx::TextureReplacements& textures, const std::vector<const ModEntry*>& active);

}  // namespace uc::mods
