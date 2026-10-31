#pragma once
#include "rows.h"
#include <functional>
#include <string>
#include <string_view>
#include <vector>
namespace uc::mods { class StandaloneMods; }
namespace uc::deck {
// What the Mods tab keeps between frames: which modal the host shows, and what it was asked about. The host (main_deck.cpp) owns the modals; the rows only ask.
struct ModsUi {
    std::string settings_id;         // the mod whose settings window is open (empty: none)
    std::string confirm_id;          // the native mod whose DLL hash the host asks the user to confirm
    std::string follow_id;           // a mod that just moved: the focus goes with it on the next frame
    bool native_warning = false;     // "Allow native code" was asked for: the host shows the warning
    bool text_open = false;          // a text setting is being edited (the host shows the entry for text_key, filled from text_buffer)
    std::string text_key, text_buffer;
    std::vector<std::string> conflict_lines;   // the files two or more mods provide, kept until the registry changes (finding them reads the mods' folders)
    bool conflicts_known = false;
    // A choice asked for its list: the host shows it and calls `choose` with the index.
    std::function<void(const std::string& title, std::vector<std::string> labels, int current, std::function<void(int)> choose)> open_list;
};
// The Mods tab as rows: "Allow native code", one row per mod (and a Confirm row under a native mod whose DLL is not confirmed yet), the file conflicts. Returns true when the
// registry changed (the host saves it).
bool draw_mods(RowCtx&, mods::StandaloneMods&, ModsUi&, std::string_view language);
// The rows of `ui.settings_id`'s declared settings and a Close row (which clears settings_id). Returns true when a value changed.
bool draw_mod_settings(RowCtx&, mods::StandaloneMods&, ModsUi&, std::string_view language);
}
