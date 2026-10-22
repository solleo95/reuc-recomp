#pragma once
#include "host.h"
#include "hooks.h"
#include "overlay.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
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

    bool active() const { return !active_.empty(); }   // at least one mod is switched on and allowed to run
    // The save folder this run starts with: <exe>/saves-modded while a mod that changes gameplay is enabled, <exe>/saves otherwise; empty when mods are
    // off or the run has an explicit root (the caller's default then applies).
    const std::filesystem::path& save_root() const { return save_root_; }
    // After the base texture folder; returns the table size. From now on apply() rebuilds this table when the manager changes the enabled mods.
    size_t load_textures(gx::TextureReplacements& table = gx::texture_replacements());
    // Builds the game API over the guest memory, starts every enabled mod (on_load) and begins delivering on_step. ui: the menu widgets (null headless).
    // The host exists whenever the mods folder holds mods, so they can be switched on while the game runs.
    void start(uint8_t* guest_memory, mods::UiApi* ui);
    void shutdown();   // on_unload, hooks removed, runtimes destroyed

    // The manager changed what is enabled (or the order, or the native-code switch): the running game follows. Menu thread. Moves the game to the modded
    // saves the first time a gameplay mod is switched on (and keeps it there until the game restarts), rebuilds the file overlay and the texture table,
    // starts and stops the mods. Returns notes for the player ("modded_saves", or a warning text).
    std::vector<std::string> apply();
    // Called when the game must move to another save folder (the SaveFs of the running game).
    // `done` runs on the game thread once the switch really happened (no save file was open); gameplay mods start only after that.
    void set_save_root_handler(std::function<void(const std::filesystem::path&, std::function<void()> done)> handler) { save_handler_ = std::move(handler); }
    // True after the game thread finished a save-folder switch: the menu thread calls apply() again so the gameplay mods start.
    bool needs_apply() const { return resync_.load(std::memory_order_relaxed); }
    // Called when the texture table has entries after a rebuild (raises the texture cache budget if it was not raised at startup).
    void set_texture_handler(std::function<void()> handler) { texture_handler_ = std::move(handler); }

    mods::ModRegistry* registry() { return registry_ ? &*registry_ : nullptr; }
    mods::ModHost* host() { return host_.get(); }
    const std::filesystem::path& mods_dir() const { return mods_dir_; }

private:
    void log(std::string_view mod_id, std::string_view line);
    static void step(void* self, uint32_t nominal);
    bool runs(const mods::ModEntry& mod) const;
    void refresh_files(bool initial);

    std::filesystem::path exe_dir_, mods_dir_;
    nlohmann::ordered_json section_;   // the config's "mods" section as loaded: the stored settings of every mod
    std::optional<mods::ModRegistry> registry_;
    std::vector<mods::ModEntry> active_;   // copies of the mods that run this session (the menu may reorder the registry later)
    std::filesystem::path save_root_;
    DvdFs* dvd_ = nullptr;
    gx::TextureReplacements* textures_ = nullptr;   // set by load_textures
    bool explicit_save_root_ = false;
    bool gameplay_mods_allowed_ = true;
    bool modded_saves_in_use_ = false;   // once the game moved to the modded saves it stays there for the session
    std::function<void(const std::filesystem::path&, std::function<void()>)> save_handler_;
    std::atomic<bool> save_ready_{true};             // the game is on the folder gameplay mods need (menu thread; set by the game thread's `done` before resync_)
    std::atomic<bool> resync_{false};
    std::vector<std::filesystem::path> files_roots_, texture_roots_;   // what the current overlay / texture table were built from
    std::function<void()> texture_handler_;
    mods::ApiRegistry api_;
    std::unique_ptr<mods::GuestMemory> memory_;
    std::shared_ptr<mods::GameApiTicker> ticker_;
    std::unique_ptr<mods::ModHost> host_;
    std::mutex log_mutex_;
    std::ofstream log_file_;                     // debug/mods.log, opened on the first line
    std::map<std::string, size_t> log_lines_;    // lines logged per mod this session (capped)
};

}  // namespace uc
