#pragma once
#include "host.h"
#include "hooks.h"
#include "overlay.h"
#include "arc/arc_mods.h"
#include "audio/audio_converter.h"
#include "text/text_mods.h"
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
    // Before the DVD session is created: the overlay must be in place before the game opens a file.
    GameMods(const std::filesystem::path& exe_dir, const nlohmann::ordered_json& mods_section, DvdFs& dvd, std::string game_id, bool disabled);
    ~GameMods();
    GameMods(const GameMods&) = delete;
    GameMods& operator=(const GameMods&) = delete;

    bool active() const { return !active_.empty(); }   // at least one mod is switched on and allowed to run
    // After the base texture folder; returns the table size. From now on apply() rebuilds this table when the manager changes the enabled mods.
    size_t load_textures(gx::TextureReplacements& table = gx::texture_replacements());
    // Builds the game API over the guest memory, starts every enabled mod (on_load) and begins delivering on_step. ui: the menu widgets (null headless).
    // The host exists whenever the mods folder holds mods, so they can be switched on while the game runs.
    void start(uint8_t* guest_memory, mods::UiApi* ui);
    void shutdown();   // on_unload, hooks removed, runtimes destroyed

    // The manager changed what is enabled (or the order, or the native-code switch): the running game follows at once. Menu thread. Rebuilds the file
    // overlay and the texture table and starts and stops the mods.
    void apply();
    // Called when the texture table has entries after a rebuild (raises the texture cache budget if it was not raised at startup).
    void set_texture_handler(std::function<void()> handler) { texture_handler_ = std::move(handler); }

    // What the Mods tab shows under a mod besides its description: audio conversion progress ("converting audio (1 of 3)") and the "audio: ...", "text: ..." and "arc: ..."
    // problems of its files; empty when there is nothing to say.
    std::string mod_status(const std::string& mod_id) const;

    mods::ModRegistry* registry() { return registry_ ? &*registry_ : nullptr; }
    mods::ModHost* host() { return host_.get(); }
    const std::filesystem::path& mods_dir() const { return mods_dir_; }

private:
    void log(std::string_view mod_id, std::string_view line);
    static void step(void* self, uint32_t nominal, ppc::Ctx* ctx);
    bool runs(const mods::ModEntry& mod) const;
    void refresh_files(bool initial);
    std::vector<std::filesystem::path> overlay_roots() const;   // per active mod: its generated audio root (when it exists), then its files/; the archive and text roots last; call with files_mutex_ held
    void audio_changed();   // the converter published or removed a file (called on its worker thread): the overlay is scanned again

    std::filesystem::path exe_dir_, mods_dir_;
    nlohmann::ordered_json section_;   // the config's "mods" section as loaded: the stored settings of every mod
    std::optional<mods::ModRegistry> registry_;
    std::vector<mods::ModEntry> active_;   // copies of the mods that run this session (the menu may reorder the registry later)
    DvdFs* dvd_ = nullptr;
    gx::TextureReplacements* textures_ = nullptr;   // set by load_textures
    std::vector<std::filesystem::path> files_roots_, texture_roots_;   // what the current overlay / texture table were built from
    std::function<void()> texture_handler_;
    mods::ApiRegistry api_;
    std::unique_ptr<mods::GuestMemory> memory_;
    std::shared_ptr<mods::GameApiTicker> ticker_;
    std::unique_ptr<mods::ModHost> host_;
    std::mutex log_mutex_;
    std::ofstream log_file_;                     // session mods.log, opened on the first line
    std::map<std::string, size_t> log_lines_;    // lines logged per mod this session (capped)
    std::map<std::string, std::vector<uint8_t>> text_bases_;   // the disc's mes_<lang>.txtres, read before any overlay (only when a mod has a text/ folder)
    std::filesystem::path text_root_;                          // the generated root of the active mods' text (guarded by files_mutex_)
    std::map<std::string, std::vector<std::string>> text_problems_;   // by mod id, from the last refresh_files
    mods::ArcMemo arc_memo_;                                   // the last archive build: a menu change that touches no archive input does not rebuild (files_mutex_)
    std::filesystem::path arc_root_;                          // the generated root of the active mods' archive members (guarded by files_mutex_)
    std::map<std::string, std::vector<std::string>> arc_problems_;    // by mod id, from the last refresh_files
    std::mutex files_mutex_;                     // active_, files_roots_ and the overlay: the menu thread (refresh_files) and the converter's worker (audio_changed)
    std::unique_ptr<mods::AudioConverter> audio_;   // last: its worker uses the members above, so it stops first
};

}  // namespace uc
