#pragma once
#include "api_registry.h"
#include <filesystem>
#include <string>
#include <vector>

namespace uc::mods {

struct PatchResult {
    size_t applied = 0;
    std::vector<std::string> errors;   // one line per value that could not be applied, naming the file and the key
};

// Applies <mod_root>/data/*.json, files in name order (a later file wins). Each file is one JSON object of API property names to values:
//
//   { "weapon.0.damage": 25, "weapon.2.capacity": 60, "player.1.invulnerable": true }
//
// A key is "<domain>.<entity>.<member>" for a scoped domain (weapon id, player slot; a whole number) or "<domain>.<member>" for a global one,
// and must name an implemented, writable member: the value goes through the member's setter exactly as a script's assignment would
// (so a weapon field patch is kept like any other). A key or value that does not work is reported and skipped; the rest still applies.
// A file over 1 MiB is skipped. Files in "data/" that are not .json are ignored; a missing folder is fine.
PatchResult apply_data_patches(const std::filesystem::path& mod_root, const ApiRegistry& api);

}  // namespace uc::mods
