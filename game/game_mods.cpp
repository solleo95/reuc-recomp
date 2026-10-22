#include "game_mods.h"
#include "profile.h"
#include <algorithm>
#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;

namespace uc {

GameMods::GameMods(const fs::path& exe_dir, const nlohmann::ordered_json& mods_section, DvdFs& dvd, std::string game_id, bool disabled, bool explicit_save_root)
    : exe_dir_(exe_dir), mods_dir_(exe_dir / "mods"), section_(mods_section.is_object() ? mods_section : nlohmann::ordered_json::object()), dvd_(&dvd), explicit_save_root_(explicit_save_root) {
    std::error_code error;
    if (disabled || !fs::is_directory(mods_dir_, error)) return;
    registry_.emplace(mods::ModRegistry::discover(mods_dir_, section_, mods::ApiVersion{1, 0}, game_id));
    for (const auto& entry : registry_->entries()) {
        if (entry.state == mods::ModState::enabled) continue;
        if (entry.state == mods::ModState::disabled) continue;
        for (const auto& message : entry.messages) std::fprintf(stderr, "[mods] %s: %s\n", entry.manifest.id.c_str(), message.c_str());
    }
    // Which saves this run uses is decided before anything of a mod is applied: if the modded saves cannot be prepared, mods that change gameplay do
    // not run at all (not even their files), so they can never touch the normal saves.
    const auto candidates = registry_->active();
    const auto choice = mods::resolve_save_root(exe_dir_, explicit_save_root_, candidates);
    save_root_ = choice.root;
    gameplay_mods_allowed_ = choice.gameplay_mods_allowed;
    if (!choice.warning.empty()) std::fprintf(stderr, "[mods] %s\n", choice.warning.c_str());
    modded_saves_in_use_ = !explicit_save_root_ && gameplay_mods_allowed_ && std::any_of(candidates.begin(), candidates.end(), [](const mods::ModEntry* mod) { return mod->manifest.touches_gameplay; });
    save_ready_ = true;   // at startup the game opens the right folder from the start
    refresh_files(true);
}

GameMods::~GameMods() { shutdown(); }

// Which enabled mods run now: gameplay ones only once the game is on the modded saves, and native ones only while native code is allowed.
bool GameMods::runs(const mods::ModEntry& mod) const {
    if (mod.manifest.touches_gameplay && (!gameplay_mods_allowed_ || !save_ready_)) return false;
    return mod.manifest.native.empty() || registry_->allow_native();
}

// Rebuilds the list of running mods, their file overlay and (once the game's texture table is known) their textures from the registry as it is now.
// The overlay is swapped in one step and only when the set of mods with files changed; likewise the texture table.
void GameMods::refresh_files(bool initial) {
    active_.clear();
    for (const auto* mod : registry_->active()) {
        if (!runs(*mod)) {
            if (initial || !(mod->manifest.touches_gameplay && gameplay_mods_allowed_)) std::fprintf(stderr, "[mods] %s is not started (%s)\n", mod->manifest.id.c_str(), mod->manifest.touches_gameplay && !gameplay_mods_allowed_ ? "it changes gameplay" : mod->manifest.touches_gameplay ? "waiting for the modded saves" : "native code is off");
            continue;
        }
        active_.push_back(*mod);
    }
    std::vector<const mods::ModEntry*> active;
    for (const auto& mod : active_) active.push_back(&mod);
    std::vector<fs::path> files_roots, texture_roots;
    for (const auto* mod : active) {
        std::error_code error;
        if (fs::is_directory(mod->root / "files", error)) files_roots.push_back(mod->root / "files");
        if (fs::is_directory(mod->root / "textures", error)) texture_roots.push_back(mod->root / "textures");
    }
    if (initial || files_roots != files_roots_) {
        files_roots_ = files_roots;
        const size_t files = dvd_->set_overlays(files_roots);
        if (!active.empty() || !initial) std::fprintf(stderr, "[mods] %zu enabled, %zu overlay files\n", active.size(), files);
    }
    for (const auto& conflict : registry_->conflicts()) {
        std::string who;
        for (const auto& id : conflict.mod_ids) who += (who.empty() ? "" : ", ") + id;
        std::fprintf(stderr, "[mods] conflict on %s: %s (the last one wins)\n", conflict.path.c_str(), who.c_str());
    }
    if (!initial && textures_ && texture_roots != texture_roots_) {   // the table is rebuilt: the base folder, then the mods in load order; the texture cache drops what it built from the old one
        texture_roots_ = texture_roots;
        textures_->clear();
        textures_->load_index(exe_dir_ / "textures");
        mods::load_mod_textures(*textures_, active);
        if (!textures_->empty() && texture_handler_) texture_handler_();
    }
}

// The manager changed something while the game runs: save folder, files, textures, then the scripts and DLLs. Returns notes for the player.
std::vector<std::string> GameMods::apply() {
    std::vector<std::string> notes;
    if (!registry_) return notes;
    resync_.store(false);
    const auto candidates = registry_->active();
    const bool wants_gameplay = std::any_of(candidates.begin(), candidates.end(), [&](const mods::ModEntry* mod) {
        return mod->manifest.touches_gameplay && (mod->manifest.native.empty() || registry_->allow_native());   // one that cannot run anyway does not move the saves
    });
    if (wants_gameplay && !explicit_save_root_ && gameplay_mods_allowed_ && !modded_saves_in_use_) {
        const auto choice = mods::resolve_save_root(exe_dir_, false, candidates);   // the modded copy of the saves is made now, from the saves as they are
        if (!choice.warning.empty()) { std::fprintf(stderr, "[mods] %s\n", choice.warning.c_str()); notes.push_back(choice.warning); }
        if (choice.gameplay_mods_allowed) {
            modded_saves_in_use_ = true;
            save_root_ = choice.root;
            notes.push_back("modded_saves");   // translated by the menu: the game now saves to the separate modded folder, until it is restarted
            // Gameplay mods start only when the game thread has really switched (no save file open): save_ready_ is set by `done`, then sync() runs again.
            if (save_handler_) { save_ready_ = false; save_handler_(choice.root, [this] { save_ready_ = true; resync_.store(true); }); }
        } else gameplay_mods_allowed_ = false;
    }
    refresh_files(false);
    if (host_) host_->sync();
    return notes;
}

size_t GameMods::load_textures(gx::TextureReplacements& table) {
    textures_ = &table;   // from now on a change in the manager rebuilds this table
    if (!active()) return 0;
    std::vector<const mods::ModEntry*> mods;
    for (const auto& mod : active_) mods.push_back(&mod);
    return mods::load_mod_textures(table, mods);
}

void GameMods::log(std::string_view mod_id, std::string_view line) {
    constexpr size_t kMaxLinesPerMod = 2000;   // per session: a mod that logs in a loop must not fill the disk
    std::lock_guard lock(log_mutex_);
    const size_t written = ++log_lines_[std::string(mod_id)];
    if (written > kMaxLinesPerMod + 1) return;
    const std::string text = written > kMaxLinesPerMod ? "log limit reached (" + std::to_string(kMaxLinesPerMod) + " lines): further lines of this mod are dropped" : std::string(line);
    std::fprintf(stderr, "[mod %.*s] %s\n", int(mod_id.size()), mod_id.data(), text.c_str());
    if (!log_file_.is_open()) {
        std::error_code error;
        fs::create_directories(exe_dir_ / "debug", error);
        log_file_.open(exe_dir_ / "debug" / "mods.log", std::ios::app);
    }
    log_file_ << '[' << mod_id << "] " << text << '\n';
    log_file_.flush();   // a crash must not lose the lines that explain it
}

void GameMods::step(void* self, uint32_t nominal) {
    if (auto* host = static_cast<GameMods*>(self)->host_.get()) host->step(nominal);
}

void GameMods::start(uint8_t* guest_memory, mods::UiApi* ui) {
    if (host_ || !registry_ || registry_->entries().empty()) return;   // with mods in the folder the host exists even if none is on: they can be switched on live
    memory_ = std::make_unique<mods::GuestMemory>(guest_memory);
    ticker_ = mods::register_game_api(api_, *memory_, mods::runtime_symbols());
    mods::ModHostConfig config;
    config.ticks.push_back([this] {   // player.invulnerable / infinite_ammo and the game events, before the mods' on_step handlers
        ticker_->tick([this](std::string_view event, const mods::Value& payload) { if (host_) host_->fire_game_event(event, payload); });
    });
    config.on_mod_dropped = [this](const std::string& id) { ticker_->drop_owner(id); };
    config.ui = ui;
    config.memory = memory_.get();
    config.include = [this](const mods::ModEntry& mod) { return (gameplay_mods_allowed_ && save_ready_) || !mod.manifest.touches_gameplay; };
    config.native.allow_native = registry_->allow_native();
    config.allow_native_now = [this] { return registry_->allow_native(); };
    config.native.confirmed = [this](const std::string& id, const std::string& hash) { return !hash.empty() && registry_->native_hash(id) == hash; };
    config.stored_settings = [this](const std::string& id) {
        const auto at = section_.find(id);
        if (at != section_.end() && !at->is_object()) log(id, "config: 'mods." + id + "' must be an object; its stored settings are ignored");
        else if (at != section_.end() && at->contains("settings") && !(*at)["settings"].is_object()) log(id, "config: 'mods." + id + ".settings' must be an object; the defaults are used");
        return at != section_.end() && at->is_object() && at->contains("settings") ? nlohmann::json((*at)["settings"]) : nlohmann::json();
    };
    config.log = [this](std::string_view id, std::string_view line) { log(id, line); };
    host_ = std::make_unique<mods::ModHost>(*registry_, api_, std::move(config));
    host_->start();
    mods::set_step_callback(&GameMods::step, this);
}

void GameMods::shutdown() {
    if (!host_) return;
    mods::set_step_callback(nullptr, nullptr);
    host_->shutdown();
    host_.reset();
    ticker_.reset();
}

}  // namespace uc
