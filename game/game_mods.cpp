#include "game_mods.h"
#include "audio/brstm.h"
#include "mod_guest_call.h"
#include <algorithm>
#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;

namespace uc {

GameMods::GameMods(const fs::path& exe_dir, const nlohmann::ordered_json& mods_section, DvdFs& dvd, std::string game_id, bool disabled)
    : exe_dir_(exe_dir), mods_dir_(exe_dir / "mods"), section_(mods_section.is_object() ? mods_section : nlohmann::ordered_json::object()), dvd_(&dvd) {
    std::error_code error;
    if (disabled || !fs::is_directory(mods_dir_, error)) return;
    registry_.emplace(mods::ModRegistry::discover(mods_dir_, section_, mods::ApiVersion{1, 1}, game_id));
    // Audio replacement: the converter learns each original stream's rate and channel count from its BRSTM header on the disc.
    audio_ = std::make_unique<mods::AudioConverter>(mods_dir_ / ".cache", [this](const std::string& stem, std::string& why) -> std::optional<mods::StreamOriginal> {
        const std::string name = "sound/stream/" + stem + ".brstm";
        const int entry = dvd_->entry_of("Data/" + name);
        if (entry < 0) { why = "the game has no stream named '" + stem + "' (" + name + ")"; return std::nullopt; }
        uint8_t head[0x100];
        const int64_t got = dvd_->read(entry, head, sizeof head, 0);
        const auto header = got < 0x40 ? std::nullopt : mods::read_brstm_header(std::span<const uint8_t>(head, size_t(got)));
        if (!header) { why = "the game's stream '" + stem + "' is not a BRSTM this port can read"; return std::nullopt; }
        if (header->channels < 1 || header->channels > 2 || header->rate == 0) { why = "the game's stream '" + stem + "' has " + std::to_string(header->channels) + " channels and " + std::to_string(header->rate) + " Hz, which the conversion cannot match"; return std::nullopt; }
        return mods::StreamOriginal{header->rate, header->channels};
    }, [this] { audio_changed(); });
    {   // the converted files of mods that were removed since the last run
        std::set<std::string> known;
        for (const auto& entry : registry_->entries()) known.insert(entry.manifest.id);
        audio_->forget_except(known);
    }
    // Text replacement: the unpatched text files are read now, before the first overlay hides them.
    bool any_text = false;
    for (const auto& entry : registry_->entries()) any_text = any_text || fs::is_directory(entry.root / "text", error);
    if (any_text) {
        for (const char* lang : mods::kTextLanguages) {
            const int entry = dvd_->entry_of(std::string("Data/message/mes_") + lang + ".txtres");
            const uint32_t length = entry < 0 ? 0 : dvd_->length(entry);
            if (length == 0 || length > (64u << 20)) continue;
            std::vector<uint8_t> bytes(length);
            if (dvd_->read(entry, bytes.data(), length, 0) == int64_t(length)) text_bases_[lang] = std::move(bytes);
        }
    }
    for (const auto& entry : registry_->entries()) {
        if (entry.state == mods::ModState::enabled) continue;
        if (entry.state == mods::ModState::disabled) continue;
        for (const auto& message : entry.messages) std::fprintf(stderr, "[mods] %s: %s\n", entry.manifest.id.c_str(), message.c_str());
    }
    refresh_files(true);
    mods::prune_text_cache(mods_dir_ / ".cache", text_root_);   // older versions of the patched text: nothing has them open yet
}

GameMods::~GameMods() { shutdown(); }

// Which enabled mods run now: native ones only while native code is allowed.
bool GameMods::runs(const mods::ModEntry& mod) const {
    return mod.manifest.native.empty() || registry_->allow_native();
}

std::vector<fs::path> GameMods::overlay_roots() const {
    std::vector<fs::path> roots;
    for (const auto& mod : active_) {
        std::error_code error;
        if (audio_) {
            const fs::path generated = audio_->generated_root(mod.manifest.id);
            if (fs::is_directory(generated / "Data", error)) roots.push_back(generated);   // before files/: a ready .brstm of the mod itself wins over its converted audio/
        }
        if (fs::is_directory(mod.root / "files", error)) roots.push_back(mod.root / "files");
    }
    if (!text_root_.empty()) roots.push_back(text_root_);   // last: the patched text files win over every mod's files
    return roots;
}

void GameMods::audio_changed() {
    std::lock_guard lock(files_mutex_);
    files_roots_ = overlay_roots();
    const size_t files = dvd_->set_overlays(files_roots_);   // scans again: the new file is not in the index yet
    std::fprintf(stderr, "[mods] audio converted: %zu overlay files\n", files);
}

// Rebuilds the list of running mods, their file overlay and (once the game's texture table is known) their textures from the registry as it is now.
// The overlay is swapped in one step and only when the set of mods with files changed; likewise the texture table.
void GameMods::refresh_files(bool initial) {
    std::lock_guard lock(files_mutex_);
    active_.clear();
    for (const auto* mod : registry_->active()) {
        if (!runs(*mod)) {
            if (initial) std::fprintf(stderr, "[mods] %s is not started (native code is off)\n", mod->manifest.id.c_str());
            continue;
        }
        active_.push_back(*mod);
    }
    std::vector<const mods::ModEntry*> active;
    for (const auto& mod : active_) active.push_back(&mod);
    if (audio_) audio_->set_active(active);   // starts converting
    {   // the text of the active mods, applied to the base files (a mod's own files/Data/message/mes_<lang>.txtres is that mod's base; the last one wins)
        const auto base = [&](const std::string& lang) -> std::optional<std::vector<uint8_t>> {
            const std::string name = "mes_" + lang + ".txtres";
            for (auto it = active.rbegin(); it != active.rend(); ++it) {
                const fs::path file = (*it)->root / "files" / "Data" / "message" / name;
                std::error_code file_error;
                const uintmax_t size = fs::is_regular_file(file, file_error) ? fs::file_size(file, file_error) : 0;
                if (size == 0 || size > (64u << 20)) continue;
                std::vector<uint8_t> bytes(size);
                std::ifstream in(file, std::ios::binary);
                if (in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) return bytes;
            }
            const auto disc = text_bases_.find(lang);
            return disc == text_bases_.end() ? std::nullopt : std::optional<std::vector<uint8_t>>(disc->second);
        };
        auto built = mods::build_text_mods(mods_dir_ / ".cache", active, base);
        text_root_ = std::move(built.root);
        text_problems_ = std::move(built.problems);
    }
    const std::vector<fs::path> files_roots = overlay_roots();
    std::vector<fs::path> texture_roots;
    for (const auto* mod : active) {
        std::error_code error;
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

// The manager changed something while the game runs: files, textures, then the scripts and DLLs.
void GameMods::apply() {
    if (!registry_) return;
    refresh_files(false);
    if (host_) host_->sync();
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

std::string GameMods::mod_status(const std::string& mod_id) const {
    std::string text = audio_ ? audio_->status(mod_id) : std::string();
    const auto problems = text_problems_.find(mod_id);
    if (problems != text_problems_.end()) for (const auto& problem : problems->second) text += (text.empty() ? "" : "; ") + problem;
    return text;
}

void GameMods::step(void* self, uint32_t nominal, ppc::Ctx* ctx) {
    mods::GuestCallScope guest(ctx);   // mods may call game functions during the step
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
