#pragma once
#include "rows.h"
#include "registry.h"
#include "settings.h"
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace uc::mods { class StandaloneMods; }
namespace uc::deck {
// What the Mods tab keeps between frames: which modal the host shows, and what it was asked about. The host owns the modals (the launcher's main_deck.cpp, or the in-game menu
// through draw_mods_windows); the rows only ask.
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
    // The list draw_mods_windows shows (the in-game menu's open_list fills it).
    struct ListModal { bool open = false; std::string title; std::vector<std::string> labels; int current = 0; std::function<void(int)> choose; } list;
};
// What the Mods tab needs of the mods behind it: the standalone manager of the launcher, or the live registry and host of the running game.
struct ModsAccess {
    std::function<mods::ModRegistry*()> registry;   // null while mods are off
    std::function<bool(const std::string& id, const std::function<void(mods::ModSettings&)>& edit)> edit;   // runs `edit` on a mod's declared settings; false for a mod without any
    std::function<std::string(const std::string& id)> dll_hash;   // sha256 of the mod's native DLL; empty when it has none
};
ModsAccess access_of(mods::StandaloneMods&);
// The Mods tab as rows: "Allow native code", one row per mod (and a Confirm row under a native mod whose DLL is not confirmed yet), the file conflicts. Returns true when the
// registry changed (the host saves it).
bool draw_mods(RowCtx&, ModsAccess&, ModsUi&, std::string_view language);
bool draw_mods(RowCtx&, mods::StandaloneMods&, ModsUi&, std::string_view language);
// The rows of `ui.settings_id`'s declared settings and a Close row (which clears settings_id). Returns true when a value changed.
bool draw_mod_settings(RowCtx&, ModsAccess&, ModsUi&, std::string_view language);
bool draw_mod_settings(RowCtx&, mods::StandaloneMods&, ModsUi&, std::string_view language);
// The windows over the Mods tab: a mod's settings (rows; its choice list and text entry open over it), the warning before native code is allowed and the DLL confirmation.
// They take every key while open; the keys of a frame go to the topmost one.
struct ModsWindows {
    float unit = 1;
    ImFont* serif = nullptr;
    FocusList* settings_focus = nullptr;   // the settings window's own focus, which stays under its list and text entry
    FocusList* modal_focus = nullptr;      // the other windows
    std::function<RowCtx(FocusList&)> make_rows;
    std::function<void()> registry_changed;   // native code allowed, a DLL confirmed
    std::function<void()> settings_changed;   // a declared setting got a new value
    std::string language;
};
bool mods_window_open(const ModsUi&);
void draw_mods_windows(ModsAccess&, ModsUi&, ModsWindows&, std::span<const DeckKey> keys);
}
