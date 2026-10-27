#pragma once
#include "data_patch.h"
#include "game_api.h"
#include <cstdint>
#include <filesystem>

namespace uc::mods {

// Identifies one mod's names while they are in effect (its address is the owner of the entries). Keep it where the mod's state lives.
struct NameUndo {
    NameUndo() = default;
    NameUndo(const NameUndo&) = delete;
    NameUndo& operator=(const NameUndo&) = delete;
};

// Applies <mod_root>/dol/names.json: the short character names the game keeps in main.dol (Ada, Billy, Carlos, Chris, HUNK, Jill, Rebecca, Wesker;
// the label table around 0x803DE4C0). One JSON object of original English name to new text:
//
//   { "Chris": "Крис", "Jill": "Джилл" }
//
// The names sit back to back in the DOL, so they are not overwritten. The game copies each one with strcpy into a 16-byte field of the character
// slot (offset 924 of its 3052-byte record, the next field starts at 940); the port hooks that strcpy and, when the source is one of these names,
// writes the mod's text there instead. The text is UTF-8 (the game's fonts read it that way), at most 15 bytes. With <mod_root>/dol/charmap.json
// (an object of one character to one character or a whole number 1-255, the font's code for that glyph) a mapped character becomes that single
// byte instead and every other character stays its UTF-8 bytes. A name that is unknown, too long, has no text, or whose place in memory does not
// look like the table (another main.dol) is reported and skipped; the rest still applies. A file over 64 KiB is skipped, a missing file is fine.
// The last mod to name a character wins; the names are used the next time the game sets its slots up (a chapter start).
PatchResult apply_name_patches(const std::filesystem::path& mod_root, GuestMemory& memory, NameUndo& owner);

// Takes back everything `owner` set.
void undo_name_patches(NameUndo& owner);

}  // namespace uc::mods
