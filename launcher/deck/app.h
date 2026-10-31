#pragma once
#include "binding_editor.h"
#include "file_browser.h"
#include "focus.h"
#include "image_unpack.h"
#include "mods_page.h"
#include "nav_keys.h"
#include "options_config.h"
#include "rows.h"
#include "settings_registry.h"
#include "standalone.h"
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
struct ImFont;
struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;
namespace uc::deck {
enum class Tab { game, graphics, controls, mods };
inline constexpr int kTabCount = 4;
// Everything the screens of the Steam Deck launcher share. Single-threaded: the UI thread owns it (the update and the unpack run on their own
// workers and are polled).
struct App {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    ImFont* sans = nullptr;
    ImFont* serif = nullptr;
    SDL_Texture* banner = nullptr;   // assets/launcher/graphite-train.png: the header
    SDL_Texture* brand = nullptr;    // assets/launcher/graphite-brand.png: the sidebar logo (0,0)-(342,446) and the footer (0,446)-(304,502)
    float unit = 1;                  // UI scale: 1 at 1280x800
    Tab tab = Tab::game;
    std::filesystem::path directory, config_path;   // the launcher's folder; config.json next to it
    ConfigDocument document;
    Options options, saved_options;
    BindingEditor pad_editor{default_pad_bindings(), BindingTable::gamepad};   // the gamepad table; options.gamepad.bindings follow it
    PadFamily pad_family = PadFamily::steam_deck;   // whose button pictures and names the bindings show (the connected pad's)
    settings::Env env;               // what this machine offers (no upscalers on the Deck; the GPU report fills in the rest)
    std::string language = "system", saved_language = "system", active_language = "en", system_language = "en";
    std::string error;               // shown on the Game tab above the Play button
    bool blocked = false;            // config.json could not be read: nothing is saved over it
    // The Game tab
    std::optional<FileBrowser> browser;              // open while the user picks the game folder or a disc image
    std::filesystem::path pending_image;             // a chosen disc image: unpacked by Play; never written to config.json (the folder stays until the unpack worked)
    std::unique_ptr<launcher::ImageUnpack> unpack;   // while an image is being unpacked into <launcher folder>/game
    std::future<std::string> features;               // the GPU report from `uc --features`, asked of uc itself while the window opens
    // Input and navigation. The D-pad drives the rows of the right part (`focus`) directly; L1/R1 pick the left side (the tabs, then the language select, which
    // is `focus.hidden()`); a modal has its own rows (`modal_focus`) and takes every key while it is open.
    KeyQueue keys;
    FocusList focus, modal_focus, settings_focus;   // settings_focus: a mod's settings window, which stays under its choice list and text entry
    TouchDrag touch;
    bool hold_capture = false;                       // the gyro's hold-button row waits for a pad press (8 s)
    int binding_sub = 0;                             // the part of the focused binding row the D-pad has selected (rows.h)
    bool reset_scroll = false;                       // a new tab starts at the top
    uint64_t capture_since_ns = 0;                   // when the gamepad binding that waits for a button started waiting (it gives up after 8 s)
    struct ListModal {                               // a choice's list (and the language list)
        bool open = false;
        std::string title;
        std::vector<std::string> labels;
        int current = 0;
        std::function<void(int)> choose;
    } list;
    std::unique_ptr<mods::StandaloneMods> mods;      // the Mods tab's registry and settings: read when the tab is first opened (finding the mods unpacks their .zip files)
    ModsUi mods_ui;
    std::string mods_error;                          // why the mods folder could not be read (the Mods tab shows it instead of the list)
    std::filesystem::path mods_directory;            // where the mods are (<launcher folder>/mods)
    bool quit_open = false;                          // "close the launcher and return to Steam?"
    bool quit = false;                               // the answer was yes
};
// Reads config.json into the App (options, language); on failure sets error/blocked and keeps the defaults.
void reload_config(App&);
// Writes the options and language back to config.json. Returns false (with app.error set) when the file cannot be saved.
bool save_config(App&, const Options& chosen);
}
