#include "arc/arc_mods.h"
#include "arc/u8.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>

namespace uc::mods {
namespace fs = std::filesystem;
namespace {
constexpr size_t kMaxProblemsPerMod = 20;
constexpr size_t kGrowthWarningBytes = 256 * 1024;

std::string utf8(const fs::path& path) {   // path::string() throws for names outside the ANSI code page
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}
fs::path from_utf8(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

uint64_t fnv1a(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    return hash;
}

std::string hex(uint64_t value) {
    char text[17];
    for (int i = 0; i < 16; ++i) text[i] = "0123456789abcdef"[(value >> (60 - 4 * i)) & 15];
    text[16] = 0;
    return text;
}

bool is_member_folder(const std::string& component) { return component.size() > 5 && lower(component.substr(component.size() - 5)) == ".arc!"; }

struct Member {
    std::string path;   // inside the archive
    fs::path file;
};
struct ModPart {
    const ModEntry* mod;
    std::vector<Member> members;
    std::optional<fs::path> whole;   // the mod's own copy of the archive
};
struct Target {
    std::string archive;   // the first spelling seen
    std::vector<ModPart> parts;   // in load order
};

std::optional<std::vector<uint8_t>> read_file(const fs::path& path, size_t limit, std::string& why) {
    std::error_code error;
    const uintmax_t size = fs::file_size(path, error);
    if (error) { why = "cannot read the file"; return std::nullopt; }
    if (size > limit) { why = "the file is larger than " + std::to_string(limit >> 20) + " MiB"; return std::nullopt; }
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes(size);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) { why = "cannot read the file"; return std::nullopt; }
    return bytes;
}
}  // namespace

ArcBuild build_arc_mods(const fs::path& cache_root, const std::vector<const ModEntry*>& mods, const ArcBase& base) {
    ArcBuild build;
    std::map<std::string, size_t> problem_count;
    const auto add_problem = [&](const std::string& mod, const std::string& text) {
        auto& list = build.problems[mod];
        if (problem_count[mod] == kMaxProblemsPerMod) list.push_back("arc: further problems of this mod are not shown");
        if (problem_count[mod]++ < kMaxProblemsPerMod) list.push_back("arc: " + text);
    };

    // 1. The members of every mod, grouped by target archive.
    std::map<std::string, Target> targets;   // by lower-case archive path
    for (const ModEntry* mod : mods) {
        const fs::path files = mod->root / "files";
        std::error_code error;
        std::map<std::string, std::vector<Member>> found;   // archive spelling -> members
        for (fs::recursive_directory_iterator it(files, error), last; !error && it != last; it.increment(error)) {
            std::error_code entry_error;
            const auto type = it->symlink_status(entry_error).type();
            if (type == fs::file_type::symlink || type == fs::file_type::junction) { it.disable_recursion_pending(); continue; }
            if (type != fs::file_type::regular) continue;
            std::vector<std::string> parts;
            for (const auto& part : it->path().lexically_relative(files)) parts.push_back(utf8(part));
            size_t cut = 0;
            while (cut < parts.size() && !is_member_folder(parts[cut])) ++cut;
            if (cut == parts.size()) continue;   // an ordinary file
            std::string archive, member;
            for (size_t i = 0; i < cut; ++i) archive += parts[i] + "/";
            archive += parts[cut].substr(0, parts[cut].size() - 1);
            bool nested = false;
            for (size_t i = cut + 1; i < parts.size(); ++i) {
                nested = nested || is_member_folder(parts[i]);
                member += (member.empty() ? "" : "/") + parts[i];
            }
            if (nested) { add_problem(mod->manifest.id, archive + "!/" + member + ": archives inside archives are not supported"); continue; }
            found[archive].push_back({member, it->path()});
        }
        for (auto& [archive, members] : found) {
            Target& target = targets[lower(archive)];
            if (target.archive.empty()) target.archive = archive;
            std::sort(members.begin(), members.end(), [](const Member& a, const Member& b) { return lower(a.path) < lower(b.path); });
            const auto same_mod = std::find_if(target.parts.begin(), target.parts.end(), [&](const ModPart& p) { return p.mod == mod; });   // two spellings of one archive
            if (same_mod != target.parts.end()) same_mod->members.insert(same_mod->members.end(), members.begin(), members.end());
            else target.parts.push_back({mod, std::move(members), std::nullopt});
        }
    }
    // A mod that ships the whole archive but no member of it is not a part of the target: its file is served by the overlay as it is, so only mods with
    // members matter; for those the whole archive (if any) is the base. Mods without members that ship the archive are looked up below.
    for (auto& [key, target] : targets) {
        std::vector<ModPart> ordered;
        for (const ModEntry* mod : mods) {
            const fs::path whole = mod->root / "files" / from_utf8(target.archive);
            std::error_code error;
            const bool has_whole = fs::symlink_status(whole, error).type() == fs::file_type::regular;   // like the overlay: a symlink is not served
            auto at = std::find_if(target.parts.begin(), target.parts.end(), [&](const ModPart& p) { return p.mod == mod; });
            if (at == target.parts.end() && !has_whole) continue;
            ModPart part = at == target.parts.end() ? ModPart{mod, {}, std::nullopt} : std::move(*at);
            if (has_whole) part.whole = whole;
            ordered.push_back(std::move(part));
        }
        target.parts = std::move(ordered);
    }

    // 2. Apply them in load order.
    std::map<std::string, std::vector<uint8_t>> produced;   // archive spelling -> bytes
    std::set<std::string> seen_conflicts;
    for (auto& [key, target] : targets) {
        std::optional<U8Archive> state;
        std::string broken_by;   // the mod whose whole archive is the current (unusable) base
        size_t base_size = 0;
        bool dropped = false;
        std::map<std::string, std::string> setter;   // lower-case member path -> mod
        size_t applied = 0;
        std::set<std::string> contributors;
        const auto blame_members = [&](const std::string& text) {
            for (const auto& part : target.parts) if (!part.members.empty()) add_problem(part.mod->manifest.id, text);
        };
        for (auto& part : target.parts) {
            const std::string& id = part.mod->manifest.id;
            if (part.whole) {
                std::string why;
                const auto bytes = read_file(*part.whole, U8Archive::kMaxArchiveBytes, why);
                std::string parse_error;
                std::optional<U8Archive> parsed = bytes ? U8Archive::parse(*bytes, parse_error) : std::nullopt;
                setter.clear();
                applied = 0;
                contributors.clear();
                if (!parsed) {   // the game would be served this damaged file: nothing can be built on it, but a later mod's whole archive replaces it
                    add_problem(id, target.archive + ": the archive of this mod cannot be used: " + (bytes ? parse_error : why));
                    state.reset();
                    broken_by = id;
                } else {
                    state = std::move(parsed);
                    base_size = bytes->size();
                    broken_by.clear();
                }
            }
            if (part.members.empty()) continue;
            if (!broken_by.empty()) {
                for (const Member& member : part.members) add_problem(id, target.archive + "!/" + member.path + ": cannot be applied: the archive of mod " + broken_by + " is damaged");
                continue;
            }
            if (!state) {
                const auto disc = base(target.archive);
                if (!disc) { blame_members(target.archive + ": the game has no such archive"); dropped = true; break; }
                std::string parse_error;
                state = U8Archive::parse(*disc, parse_error);
                if (!state) { blame_members(target.archive + ": cannot be used as an archive: " + parse_error); dropped = true; break; }
                base_size = disc->size();
            }
            for (const Member& member : part.members) {
                const std::string where = target.archive + "!/" + member.path;
                std::string why;
                const auto data = read_file(member.file, U8Archive::kMaxMemberBytes, why);
                if (!data) { add_problem(id, where + ": " + why); continue; }
                std::string error;
                if (!state->add_or_replace(member.path, *data, error)) { add_problem(id, where + ": " + error); continue; }
                auto& previous = setter[lower(member.path)];
                if (!previous.empty() && previous != id) {
                    const std::string conflict = "archive member " + where + " is set by " + previous + " and " + id + " (the last one wins)";
                    if (seen_conflicts.insert(conflict).second) build.conflicts.push_back(conflict);
                }
                previous = id;
                ++applied;
                contributors.insert(id);
            }
        }
        if (dropped || !state || applied == 0) continue;
        std::vector<uint8_t> out = state->write();
        if (out.size() > U8Archive::kMaxArchiveBytes) {
            for (const auto& id : contributors) add_problem(id, target.archive + ": the changed archive would be larger than 256 MiB");
            continue;
        }
        if (out.size() > base_size + kGrowthWarningBytes) {
            const std::string text = target.archive + ": " + std::to_string((out.size() - base_size) / 1024) + " KiB bigger than the original; the game may run out of memory loading it";
            for (const auto& id : contributors) add_problem(id, text);
        }
        produced[target.archive] = std::move(out);
    }
    if (produced.empty()) return build;

    // 3. The generated root, named by the content.
    uint64_t hash = 1469598103934665603ull;
    for (const auto& [archive, bytes] : produced) {
        const std::string name = lower(archive);
        hash = fnv1a(hash, name.data(), name.size());
        hash = fnv1a(hash, bytes.data(), bytes.size());
    }
    const fs::path root = cache_root / "_arc" / hex(hash);
    const auto complete = [&] {   // a folder of this key from an earlier run: every file there and as long as it should be (a cut-short copy is rebuilt)
        for (const auto& [archive, bytes] : produced) {
            std::error_code error;
            const uintmax_t size = fs::file_size(root / from_utf8(archive), error);
            if (error || size != bytes.size()) return false;
        }
        return true;
    };
    if (!complete()) {
        std::error_code error;
        fs::path temp = root;
        temp += ".tmp";
        fs::remove_all(temp, error);
        bool ok = true;
        for (const auto& [archive, bytes] : produced) {
            const fs::path file = temp / from_utf8(archive);
            fs::create_directories(file.parent_path(), error);
            ok = ok && !error;
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            out.close();   // the buffered tail is written here: a full disk shows up only now
            ok = ok && !out.fail();
        }
        if (ok) { fs::remove_all(root, error); fs::rename(temp, root, error); ok = !error; }
        if (!ok) {
            fs::remove_all(temp, error);
            for (const auto& [key, target] : targets) for (const auto& part : target.parts) if (!part.members.empty()) add_problem(part.mod->manifest.id, "the changed archives cannot be written (disk full?)");
            return build;
        }
    }
    build.root = root;
    return build;
}

void prune_arc_cache(const fs::path& cache_root, const fs::path& keep) {
    std::error_code error;
    const fs::path dir = cache_root / "_arc";
    std::vector<fs::path> remove;
    for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error))
        if (it->path().lexically_normal() != keep.lexically_normal()) remove.push_back(it->path());
    for (const auto& path : remove) fs::remove_all(path, error);
}

}  // namespace uc::mods
