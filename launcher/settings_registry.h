#pragma once
#include "backend_ui.h"
#include "options.h"
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace uc::settings {
// The one table of settings. For every setting it says where the setting shows (the desktop in-game menu, the Steam Deck in-game menu, the desktop launcher, the Deck
// launcher) and, for the simple kinds, how to read, change and label it, so a menu can draw it without a line of its own. The desktop menu and the launcher keep their
// hand-written widgets and only ask `shown()`; the Deck menu draws every item that has its bit. A new setting is one row in settings_registry.cpp.
enum Surface : uint8_t { desktop_menu=1, deck_menu=2, desktop_launcher=4, deck_launcher=8 };
constexpr uint8_t kDesktop=desktop_menu|desktop_launcher, kDeck=deck_menu|deck_launcher, kMenus=desktop_menu|deck_menu, kEverywhere=15;
enum class Page : uint8_t { game, graphics, effects, controls, gamepad, buttons, reticle, mods };   // buttons and mods are drawn by the Deck menu itself (one custom item each)
enum class Kind : uint8_t { toggle, choice, slider, color, custom };   // custom: listed for its surfaces, drawn by the surface's own code
// What the machine and the run offer (the feature report, the graphics API, the monitor): items are greyed out or hidden by it.
struct Env {
    bool features_known=false;          // the feature report has arrived; until then nothing is blocked
    bool effects=true;                  // the GPU runs the screen-space effects
    uint32_t anisotropy_max=16;
    bool vendor_upscalers=false;        // DLSS / FSR can be chosen (not on the Steam Deck)
    uint32_t monitor_width=1920, monitor_height=1080;
    BackendUi backend=backend_ui(RendererBackend::vulkan);
};
using Text=std::function<std::string(std::string_view key)>;   // a translated string
struct State { bool visible=true, enabled=true; const char* why=nullptr; };   // why: translation key shown for a greyed-out item
// An item's colour presets (kind color): 0 is "the game's own".
struct Item {
    const char* id; const char* label_key; const char* hint_key;   // hint_key may be null
    Page page; Kind kind; uint8_t surfaces;
    State (*state)(const Options&,const Env&)=nullptr;   // null: always visible and enabled
    // toggle
    bool (*get_bool)(const Options&)=nullptr; void (*set_bool)(Options&,bool)=nullptr;
    // slider: an integer in [min,max] moved by `step`; `format` is printf-style with one %d
    int (*get_int)(const Options&)=nullptr; void (*set_int)(Options&,int)=nullptr; int min=0, max=0, step=1; const char* format="%d";
    // choice: the labels in order, the index of the current value (-1: a value the list does not hold) and the selection
    std::vector<std::string> (*labels)(const Options&,const Env&,const Text&)=nullptr;
    int (*index)(const Options&,const Env&)=nullptr; void (*select)(Options&,const Env&,int)=nullptr;
    // color: 0xRRGGBB, 0 = the game's own
    uint32_t (*get_color)(const Options&)=nullptr; void (*set_color)(Options&,uint32_t)=nullptr;
};
std::span<const Item> items();
const Item* find(std::string_view id);   // nullptr for an id the table does not hold
bool on_surface(const Item&,Surface);
// Whether `id` shows on `surface` with these options: its bit is set and its state says visible. An unknown id throws std::logic_error (a typo in a menu must not hide a setting silently).
bool shown(std::string_view id,Surface,const Options&,const Env&);
bool shown(std::string_view id,Surface);   // the bit only
std::vector<const Item*> items_for(Surface,Page);   // table order
std::vector<Page> pages_for(Surface);               // the pages that have an item for the surface, in page order
const char* page_label_key(Page);
// The colour presets of a color item, 0xRRGGBB (the first is 0: the game's own).
std::span<const uint32_t> color_presets();
// What a pad does to an item: toggles flip, choices move to the neighbour (no wrap), sliders move by their step, colours move through the presets. delta is -1 or +1.
// Returns true when the options changed.
bool step(const Item&,Options&,const Env&,int delta);
// Toggle: flips. Choice and colour: no-op here (the menu opens its list). Returns true when the options changed.
bool flip(const Item&,Options&);
// A slider set to `value` (clamped to its range, snapped to its step); a choice to index `i`; a toggle to `on`. They return true when the options changed.
bool set_slider(const Item&,Options&,int value);
bool set_choice(const Item&,Options&,const Env&,int index);
std::string slider_text(const Item&,const Options&);   // the value as the item formats it
}
