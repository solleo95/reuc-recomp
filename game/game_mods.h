#pragma once
#include "host.h"
#include "hooks.h"
#include "overlay.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace uc {

// The port's side of the mod platform (uc_main wires it in): finds <exe folder>/mods, applies the enabled mods' file overlays to the disc
// tree, indexes their textures, and runs their scripts and DLLs through a mods::ModHost for the length of the game. Everything is skipped
// (nothing is scanned, no runtime exists) when mods are switched off, the folder is missing, or no mod is enabled.
class GameMods {
public:
    // Before the DVD session is created: the overlay must be in place before the game opens a file. explicit_save_root: the run has its own
    // --save-root (the test harness), which is never redirected to the modded saves.
    GameMods(const std::filesystem::path& exe_dir, const nlohmann::ordered_json& mods_section, DvdFs& dvd, std::string game_id, bool disabled, bool explicit_save_root = false);
    ~GameMods();
    GameMods(const GameMods&) = delete;
    GameMods& operator=(const GameMods&) = delete;

    bool active() const { return !active_.empty(); }
    // The save folder this run must use: <exe>/saves-modded while a mod that changes gameplay is active, <exe>/saves otherwise; empty when mods are
    // off or the run has an explicit root (the caller's default then applies).
    const std::filesystem::path& save_root() const { return save_root_; }
    size_t load_textures(gx::TextureReplacements& table = gx::texture_replacements());   // after the base texture folder; returns the table size
    // Builds the game API over the guest memory, starts every active mod (on_load) and begins delivering on_step. ui: the menu widgets (null headless).
    void start(uint8_t* guest_memory, mods::UiApi* ui);
    void shutdown();   // on_unload, hooks removed, runtimes destroyed

    mods::ModRegistry* registry() { return registry_ ? &*registry_ : nullptr; }
    mods::ModHost* host() { return host_.get(); }
    const std::filesystem::path& mods_dir() const { return mods_dir_; }

private:
    void log(std::string_view mod_id, std::string_view line);
    static void step(void* self, uint32_t nominal);

    std::filesystem::path exe_dir_, mods_dir_;
    nlohmann::ordered_json section_;   // the config's "mods" section as loaded: the stored settings of every mod
    std::optional<mods::ModRegistry> registry_;
    std::vector<mods::ModEntry> active_;   // copies of the mods that run this session (the menu may reorder the registry later)
    std::filesystem::path save_root_;
    bool gameplay_mods_allowed_ = true;
    mods::ApiRegistry api_;
    std::unique_ptr<mods::GuestMemory> memory_;
    std::shared_ptr<mods::GameApiTicker> ticker_;
    std::unique_ptr<mods::ModHost> host_;
    std::mutex log_mutex_;
    std::ofstream log_file_;                     // debug/mods.log, opened on the first line
    std::map<std::string, size_t> log_lines_;    // lines logged per mod this session (capped)
};

}  // namespace uc
