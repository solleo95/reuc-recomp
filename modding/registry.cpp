#include "registry.h"
#include "mod_paths.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <optional>

namespace fs = std::filesystem;
using nlohmann::ordered_json;

namespace uc::mods {
namespace {
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::vector<long> version_parts(std::string_view text) {
    std::vector<long> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const auto dot = text.find('.', start);
        const std::string piece(text.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
        long value = 0;
        for (char c : piece) { if (c < '0' || c > '9') { value = 0; break; } value = value * 10 + (c - '0'); if (value > 1000000000) break; }
        parts.push_back(value);
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return parts;
}

// Files below dir as lower-case '/' paths relative to dir.
void list_files(const fs::path& dir, std::vector<std::string>& out) {
    std::error_code error;
    for (fs::recursive_directory_iterator it(dir, error), last; !error && it != last; it.increment(error)) {
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error)) continue;
        const auto rel = it->path().lexically_relative(dir).generic_u8string();
        out.push_back(lower(std::string(reinterpret_cast<const char*>(rel.data()), rel.size())));
    }
}
}  // namespace

int compare_versions(std::string_view a, std::string_view b) {
    auto pa = version_parts(a), pb = version_parts(b);
    const size_t n = std::max(pa.size(), pb.size());
    pa.resize(n, 0); pb.resize(n, 0);
    for (size_t i = 0; i < n; ++i) if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    return 0;
}

ModRegistry ModRegistry::discover(const fs::path& mods_dir, const ordered_json& mods_section, ApiVersion host_api, std::string_view game_version) {
    ModRegistry registry;
    registry.section_ = mods_section.is_object() ? mods_section : ordered_json::object();
    registry.host_api_ = host_api;
    registry.game_version_ = std::string(game_version);
    const auto& section = registry.section_;
    if (section.contains("enabled") && section["enabled"].is_array())
        for (const auto& id : section["enabled"]) if (id.is_string()) registry.wanted_.insert(id.get<std::string>());
    registry.allow_native_ = section.contains("allow_native") && section["allow_native"].is_boolean() && section["allow_native"].get<bool>();

    std::set<std::string> seen;
    for (auto& found : find_mod_folders(mods_dir)) {
        ModEntry entry;
        entry.folder_name = found.folder_name;
        entry.root = found.root;
        entry.manifest.id = entry.manifest.name = found.folder_name;
        bool broken = true;
        if (!found.error.empty()) entry.messages.push_back(found.error);
        else {
            auto parsed = parse_manifest(found.manifest_text);
            if (!parsed.manifest) entry.messages = std::move(parsed.errors);
            else if (!seen.insert(parsed.manifest->id).second) entry.messages.push_back("duplicate id '" + parsed.manifest->id + "': another mod folder already uses it");
            else { entry.manifest = std::move(*parsed.manifest); broken = false; }
        }
        if (broken) entry.state = ModState::error;
        registry.entries_.push_back(std::move(entry));
        registry.broken_.push_back(broken);
    }

    // Saved load order first, then the mods the saved order does not know (alphabetical by folder, as discovered).
    std::vector<std::string> saved_order;
    if (section.contains("order") && section["order"].is_array())
        for (const auto& id : section["order"]) if (id.is_string()) saved_order.push_back(id.get<std::string>());
    std::vector<size_t> index(registry.entries_.size());
    for (size_t i = 0; i < index.size(); ++i) index[i] = i;
    const auto rank = [&](size_t i) {
        const auto at = std::find(saved_order.begin(), saved_order.end(), registry.entries_[i].manifest.id);
        return at == saved_order.end() ? saved_order.size() : size_t(at - saved_order.begin());
    };
    std::stable_sort(index.begin(), index.end(), [&](size_t a, size_t b) { return rank(a) < rank(b); });
    std::vector<ModEntry> ordered;
    std::vector<bool> ordered_broken;
    for (size_t i : index) { ordered.push_back(std::move(registry.entries_[i])); ordered_broken.push_back(registry.broken_[i]); }
    registry.entries_ = std::move(ordered);
    registry.broken_ = std::move(ordered_broken);
    registry.evaluate();
    return registry;
}

void ModRegistry::evaluate() {
    std::map<std::string, size_t> by_id;
    for (size_t i = 0; i < entries_.size(); ++i) if (!broken_[i]) by_id[entries_[i].manifest.id] = i;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (broken_[i]) continue;
        auto& e = entries_[i];
        e.messages.clear();
        e.state = ModState::disabled;
        if (!wanted_.count(e.manifest.id)) continue;
        const auto& m = e.manifest;
        if (m.api.major != host_api_.major || m.api.minor > host_api_.minor) {
            e.state = ModState::incompatible;
            e.messages.push_back("needs mod API " + std::to_string(m.api.major) + "." + std::to_string(m.api.minor) + ", this game offers " + std::to_string(host_api_.major) + "." + std::to_string(host_api_.minor));
        } else if (!m.game.empty() && std::find(m.game.begin(), m.game.end(), game_version_) == m.game.end()) {
            e.state = ModState::incompatible;
            e.messages.push_back("made for another game version than " + game_version_);
        } else e.state = ModState::enabled;
    }
    for (bool changed = true; changed;) {   // dependency problems spread to dependents until nothing changes; cycles are cut first
        changed = false;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (broken_[i] || entries_[i].state != ModState::enabled) continue;
            auto& e = entries_[i];
            for (const auto& dep : e.manifest.depends) {
                const auto at = by_id.find(dep.id);
                if (at == by_id.end()) { e.state = ModState::missing_dependency; e.messages.push_back("requires '" + dep.id + "', which is not installed"); }
                else if (entries_[at->second].state != ModState::enabled) { e.state = ModState::dependency_disabled; e.messages.push_back("requires '" + dep.id + "', which is not enabled or not working"); }
                else if (!dep.min_version.empty() && compare_versions(entries_[at->second].manifest.version, dep.min_version) < 0) {
                    e.state = ModState::missing_dependency; e.messages.push_back("requires '" + dep.id + "' " + dep.min_version + " or newer");
                }
                if (e.state != ModState::enabled) { changed = true; break; }
            }
        }
        if (changed) continue;
        // A cycle: an enabled mod that can reach itself through enabled dependencies.
        std::vector<size_t> in_cycle;   // all members are found before any is marked, so every one of them reports the cycle
        for (size_t start = 0; start < entries_.size(); ++start) {
            if (broken_[start] || entries_[start].state != ModState::enabled) continue;
            std::vector<size_t> stack{start};
            std::set<size_t> visited;
            bool cycle = false;
            while (!stack.empty() && !cycle) {
                const size_t at = stack.back(); stack.pop_back();
                for (const auto& dep : entries_[at].manifest.depends) {
                    const auto next = by_id.find(dep.id);
                    if (next == by_id.end() || entries_[next->second].state != ModState::enabled) continue;
                    if (next->second == start) { cycle = true; break; }
                    if (visited.insert(next->second).second) stack.push_back(next->second);
                }
            }
            if (cycle) in_cycle.push_back(start);
        }
        for (size_t i : in_cycle) { entries_[i].state = ModState::error; entries_[i].messages.push_back("dependency cycle through '" + entries_[i].manifest.id + "'"); changed = true; }
    }
}

std::vector<const ModEntry*> ModRegistry::active() const {
    std::vector<const ModEntry*> result;
    std::set<std::string> emitted;
    std::vector<bool> done(entries_.size(), false);
    for (bool progress = true; progress;) {
        progress = false;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (done[i] || broken_[i] || entries_[i].state != ModState::enabled) continue;
            const bool ready = std::all_of(entries_[i].manifest.depends.begin(), entries_[i].manifest.depends.end(), [&](const ModDependency& d) { return emitted.count(d.id) > 0; });
            if (!ready) continue;
            done[i] = true; emitted.insert(entries_[i].manifest.id); result.push_back(&entries_[i]); progress = true;
            break;   // restart from the top so load order breaks ties
        }
    }
    return result;
}

bool ModRegistry::set_enabled(std::string_view id, bool on) {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (broken_[i] || entries_[i].manifest.id != id) continue;
        if (on) wanted_.insert(std::string(id)); else wanted_.erase(std::string(id));
        evaluate();
        return true;
    }
    return false;
}

bool ModRegistry::move(std::string_view id, int delta) {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].manifest.id != id) continue;
        const long target = long(i) + delta;
        if (target < 0 || target >= long(entries_.size()) || delta == 0) return false;
        auto entry = std::move(entries_[i]);
        const bool broken = broken_[i];
        entries_.erase(entries_.begin() + long(i)); broken_.erase(broken_.begin() + long(i));
        entries_.insert(entries_.begin() + target, std::move(entry)); broken_.insert(broken_.begin() + target, broken);
        return true;
    }
    return false;
}

std::vector<Conflict> ModRegistry::conflicts() const {
    std::map<std::string, std::vector<std::string>> providers;
    for (const auto* mod : active()) {
        for (const char* folder : {"files", "textures"}) {
            std::vector<std::string> files;
            list_files(mod->root / folder, files);
            for (auto& rel : files) providers[std::string(folder) + "/" + rel].push_back(mod->manifest.id);
        }
    }
    std::vector<Conflict> result;
    for (auto& [path, ids] : providers) if (ids.size() > 1) result.push_back({path, std::move(ids)});
    return result;
}

ordered_json ModRegistry::to_config() const {
    ordered_json config = section_;
    config["enabled"] = ordered_json::array();
    config["order"] = ordered_json::array();
    std::set<std::string> listed;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& id = entries_[i].manifest.id;
        config["order"].push_back(id);
        if (wanted_.count(id)) { config["enabled"].push_back(id); listed.insert(id); }
    }
    for (const auto& id : wanted_) if (!listed.count(id)) config["enabled"].push_back(id);   // enabled but currently not installed: remembered
    config["allow_native"] = allow_native_;
    if (!config.contains("native_hash") || !config["native_hash"].is_object()) config["native_hash"] = ordered_json::object();
    return config;
}

std::string ModRegistry::native_hash(std::string_view id) const {
    if (!section_.contains("native_hash") || !section_["native_hash"].is_object()) return {};
    const auto at = section_["native_hash"].find(std::string(id));
    return at != section_["native_hash"].end() && at->is_string() ? at->get<std::string>() : std::string();
}

void ModRegistry::set_native_hash(std::string_view id, std::string hash) {
    if (!section_.contains("native_hash") || !section_["native_hash"].is_object()) section_["native_hash"] = ordered_json::object();
    section_["native_hash"][std::string(id)] = std::move(hash);
}

}  // namespace uc::mods
