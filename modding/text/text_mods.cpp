#include "text/text_mods.h"
#include "text/txtres.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <set>
#include <tuple>
#include <system_error>

namespace uc::mods {
namespace fs = std::filesystem;
namespace {
constexpr size_t kMaxJsonBytes = 8 * 1024 * 1024, kMaxLineBytes = 2048, kMaxLines = 32;

std::string utf8(const fs::path& path) {   // path::string() throws for names outside the ANSI code page
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

bool is_language(const std::string& name) { return std::find(std::begin(kTextLanguages), std::end(kTextLanguages), name) != std::end(kTextLanguages); }

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

// The printf conversions of a text ("%s%d" for "a %s b %d"): the game hands some records to printf, so a text may not add, drop or change them. "%%" is a
// percent sign; a '%' that starts no valid conversion counts as "?" (it would be one for printf: "100% damage" is "% d").
std::string conversions(const std::string& text) {
    std::string result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') continue;
        if (++i >= text.size()) { result += '?'; break; }
        if (text[i] == '%') continue;
        while (i < text.size() && std::strchr("-+ #0", text[i])) ++i;
        while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '*')) ++i;
        if (i < text.size() && text[i] == '.') { ++i; while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '*')) ++i; }
        while (i < text.size() && std::strchr("hlLqjzt", text[i])) ++i;
        result += i < text.size() && std::strchr("diouxXeEfgGaAcspn", text[i]) ? text[i] : '?';
    }
    return result;
}

std::string joined(const std::vector<std::string>& lines) {
    std::string text;
    for (const auto& line : lines) text += line;
    return text;
}

struct Parsed {
    std::string file;   // "us.json"
    TextChanges changes;
};

constexpr size_t kMaxEntries = 100000, kMaxProblemsPerMod = 20, kShownKey = 32;

// A JSON key as it is printed in a message: cut at 32 bytes (never inside a UTF-8 character), so a hostile key cannot fill the Mods tab.
std::string shown(const std::string& key) {
    if (key.size() <= kShownKey) return key;
    size_t cut = kShownKey;
    while (cut > 0 && (static_cast<unsigned char>(key[cut]) & 0xC0) == 0x80) --cut;
    return key.substr(0, cut) + "...";
}

// "a, b, c, d, e, ..." for the first five of a list of ids.
std::string some(const std::vector<uint32_t>& ids) {
    std::string text;
    for (size_t i = 0; i < ids.size() && i < 5; ++i) text += (i ? ", " : "") + std::to_string(ids[i]);
    return ids.size() > 5 ? text + ", ..." : text;
}

std::string ids_phrase(size_t count) { return count == 1 ? "1 id" : std::to_string(count) + " ids"; }

// One text/<name>.json; problems are appended as "text: <file>: ...". Bad entries are skipped, the rest is kept.
Parsed parse_file(const fs::path& path, std::vector<std::string>& problems) {
    Parsed parsed{utf8(path.filename()), {}};
    const auto problem = [&](const std::string& text) { problems.push_back("text: " + parsed.file + ": " + text); };
    std::error_code error;
    const uintmax_t size = fs::file_size(path, error);
    if (error) { problem("cannot read the file"); return parsed; }
    if (size > kMaxJsonBytes) { problem("the file is larger than 8 MB"); return parsed; }
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded()) { problem("not valid JSON"); return parsed; }
    if (!json.is_object()) { problem("must be a JSON object of record ids to text"); return parsed; }
    if (json.size() > kMaxEntries) { problem("more than " + std::to_string(kMaxEntries) + " entries"); return parsed; }
    for (const auto& [key, value] : json.items()) {
        const std::string id_text = "id \"" + shown(key) + "\"";
        const bool digits = !key.empty() && key.size() <= 10 && std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        if (!digits || std::stoull(key) > 0xFFFFFFFFull) { problem(id_text + " is not a record id (a whole number)"); continue; }
        if (key.size() > 1 && key[0] == '0') { problem(id_text + " has leading zeros: write the id without them"); continue; }   // "007" and "7" would be one record
        if (!value.is_string()) { problem(id_text + " must be text"); continue; }
        const std::string& s = value.get_ref<const std::string&>();
        if (s.find('\0') != std::string::npos) { problem(id_text + ": the text holds a NUL character"); continue; }
        std::vector<std::string> lines;
        size_t start = 0;
        for (size_t at; (at = s.find('\n', start)) != std::string::npos; start = at + 1) lines.push_back(s.substr(start, at - start));
        lines.push_back(s.substr(start));
        for (auto& line : lines) if (!line.empty() && line.back() == '\r') line.pop_back();   // Windows line ends: the game's texts have none
        if (lines.size() > kMaxLines) { problem(id_text + ": more than " + std::to_string(kMaxLines) + " lines"); continue; }
        if (std::any_of(lines.begin(), lines.end(), [](const std::string& l) { return l.size() > kMaxLineBytes; })) { problem(id_text + ": a line is longer than " + std::to_string(kMaxLineBytes) + " bytes"); continue; }
        parsed.changes[uint32_t(std::stoull(key))] = std::move(lines);
    }
    return parsed;
}

struct Origin { std::string mod, file; };

// What the base file says about its records: the printf conversions of each text, and the largest line and line count (to notice a text far beyond them).
struct OriginalInfo {
    std::map<uint32_t, std::string> conversions;
    size_t max_line = 0, max_lines = 0;
};
}  // namespace

TextBuild build_text_mods(const fs::path& cache_root, const std::vector<const ModEntry*>& mods, const TextBase& base) {
    TextBuild build;
    std::map<std::string, TextChanges> changes;                                  // language -> merged changes of all mods
    std::map<std::string, std::map<uint32_t, std::vector<Origin>>> origin;       // language -> id -> everybody who set it, in order
    std::map<std::string, std::set<std::string>> contributors;                   // language -> mods
    std::map<std::string, std::set<std::string>> seen_problems;                  // mod -> problems already listed (the same text is not listed twice)
    std::set<std::string> seen_conflicts;
    const auto add_problem = [&](const std::string& mod, const std::string& text) {
        if (!seen_problems[mod].insert(text).second) return;
        auto& list = build.problems[mod];
        if (list.size() < kMaxProblemsPerMod) list.push_back(text);
        else if (list.size() == kMaxProblemsPerMod) list.push_back("text: more problems are not listed");   // the Mods tab is not a log
    };
    const auto add_problems = [&](const std::string& mod, const std::vector<std::string>& texts) { for (const auto& t : texts) add_problem(mod, t); };

    std::map<std::string, std::optional<TextBaseFile>> bases;   // looked up once per language
    std::map<std::string, OriginalInfo> originals;
    const auto base_of = [&](const std::string& lang) -> const std::optional<TextBaseFile>& {
        auto it = bases.find(lang);
        if (it == bases.end()) it = bases.emplace(lang, base(lang)).first;
        return it->second;
    };
    const auto original_of = [&](const std::string& lang) -> const OriginalInfo& {
        auto it = originals.find(lang);
        if (it != originals.end()) return it->second;
        OriginalInfo info;
        std::string ignored;   // a damaged base is reported when it is patched
        if (const auto records = read_txtres(base_of(lang)->bytes, ignored)) {
            for (const auto& r : *records) {
                info.conversions[r.id] = conversions(joined(r.lines));
                info.max_lines = std::max(info.max_lines, r.lines.size());
                for (const auto& line : r.lines) info.max_line = std::max(info.max_line, line.size());
            }
        }
        return originals.emplace(lang, std::move(info)).first->second;
    };

    for (const ModEntry* mod : mods) {
        const std::string& id = mod->manifest.id;
        std::vector<fs::path> files;
        std::error_code error;
        for (fs::directory_iterator it(mod->root / "text", error), end; !error && it != end; it.increment(error)) {
            std::error_code entry_error;
            if (it->is_regular_file(entry_error) && !it->is_symlink(entry_error) && lower(utf8(it->path().extension())) == ".json") files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        std::optional<Parsed> all;
        std::map<std::string, Parsed> by_language;
        std::vector<std::string> problems;
        for (const auto& path : files) {
            const std::string stem = lower(utf8(path.stem()));
            if (stem != "all" && !is_language(stem)) { problems.push_back("text: " + utf8(path.filename()) + ": '" + shown(stem) + "' is not a language (us, en, fr, ge, it, jp, sp, du) or 'all'"); continue; }
            Parsed parsed = parse_file(path, problems);
            if (stem == "all") all = std::move(parsed); else by_language[stem] = std::move(parsed);
        }
        add_problems(id, problems);
        const auto apply = [&](const std::string& lang, const Parsed& parsed) {
            const OriginalInfo& original = original_of(lang);
            bool any = false;
            for (const auto& [record, lines] : parsed.changes) {
                const std::string where = "text: " + parsed.file + ": id \"" + std::to_string(record) + "\"";
                const auto expected = original.conversions.find(record);
                if (expected != original.conversions.end()) {
                    if (conversions(joined(lines)) != expected->second) {
                        add_problem(id, where + ": the placeholders (%s, %d, ...) must match the original's, which has \"" + expected->second + "\" (write %% for a percent sign); the original text is kept");
                        continue;
                    }
                    size_t longest = 0;
                    for (const auto& line : lines) longest = std::max(longest, line.size());
                    if ((original.max_line && longest > 2 * original.max_line) || (original.max_lines && lines.size() > 2 * original.max_lines))   // applied, but the game's text boxes were made for the original's sizes
                        add_problem(id, where + ": much longer than the original text file has (a line of " + std::to_string(longest) + " bytes and " + std::to_string(lines.size()) + " lines against at most " + std::to_string(original.max_line) + " and " + std::to_string(original.max_lines) + "): it may not fit the game's text boxes");
                }
                auto& setters = origin[lang][record];
                if (!setters.empty() && setters.back().mod != id) {
                    const std::string conflict = "text id " + std::to_string(record) + " is set by " + setters.back().mod + " and " + id + " (the last one wins)";
                    if (seen_conflicts.insert(conflict).second) build.conflicts.push_back(conflict);
                }
                setters.push_back({id, parsed.file});
                changes[lang][record] = lines;
                any = true;
            }
            if (any) contributors[lang].insert(id);
        };
        for (const char* lang_name : kTextLanguages) {
            const std::string lang = lang_name;
            const bool has_language_file = by_language.count(lang) && !by_language[lang].changes.empty();
            if (all && !all->changes.empty() && base_of(lang)) apply(lang, *all);   // all.json reaches every language the game has
            if (has_language_file) {
                if (!base_of(lang)) add_problem(id, "text: " + by_language[lang].file + ": the game has no text file for the language '" + lang + "'");
                else apply(lang, by_language[lang]);
            }
        }
        if (all && !all->changes.empty() && std::none_of(std::begin(kTextLanguages), std::end(kTextLanguages), [&](const char* l) { return base_of(l).has_value(); }))
            add_problem(id, "text: all.json: the game has no text file to change");
    }

    std::map<std::string, std::vector<uint8_t>> produced;   // language -> patched file
    std::map<std::pair<std::string, uint32_t>, size_t> all_unknown;   // (mod, id) set by all.json -> languages where it does not exist
    std::map<std::tuple<std::string, std::string, std::string>, std::vector<uint32_t>> unknown_in_file;   // (mod, file, language) -> ids of that file the language lacks
    size_t patched_languages = 0;
    for (auto& [lang, merged] : changes) {
        std::string error;
        const auto patch = patch_txtres(base_of(lang)->bytes, merged, error);
        if (!patch) {
            for (const auto& mod : contributors[lang]) add_problem(mod, "text: mes_" + lang + ".txtres cannot be changed: " + error);
            if (!base_of(lang)->from_mod.empty())   // the base is a mod's own file: that mod gets the blame too
                add_problem(base_of(lang)->from_mod, "text: files/Data/message/mes_" + lang + ".txtres cannot be used as the base for text changes: " + error);
            continue;
        }
        ++patched_languages;
        for (uint32_t unknown : patch->unknown_ids) {
            for (const Origin& from : origin[lang][unknown]) {
                if (lower(from.file) == "all.json") ++all_unknown[{from.mod, unknown}];
                else unknown_in_file[{from.mod, from.file, lang}].push_back(unknown);
            }
        }
        if (patch->unknown_ids.size() == merged.size()) continue;   // nothing of it applies: no file
        produced[lang] = patch->file;
    }
    for (const auto& [key, ids] : unknown_in_file)
        add_problem(std::get<0>(key), "text: " + std::get<1>(key) + ": " + ids_phrase(ids.size()) + (ids.size() == 1 ? " does" : " do") + " not exist in mes_" + std::get<2>(key) + ".txtres (" + some(ids) + ")");
    std::map<std::string, std::vector<uint32_t>> nowhere;   // mod -> ids of all.json that no language has
    for (const auto& [key, languages] : all_unknown) if (languages == patched_languages) nowhere[key.first].push_back(key.second);
    for (const auto& [mod, ids] : nowhere)
        add_problem(mod, "text: all.json: " + ids_phrase(ids.size()) + (ids.size() == 1 ? " does" : " do") + " not exist in any language (" + some(ids) + ")");
    if (produced.empty()) return build;

    uint64_t hash = 1469598103934665603ull;
    for (const auto& [lang, bytes] : produced) { hash = fnv1a(hash, lang.data(), lang.size()); hash = fnv1a(hash, bytes.data(), bytes.size()); }
    const fs::path root = cache_root / "_text" / hex(hash);
    std::error_code error;
    const auto complete = [&] {   // a folder of this key from an earlier run: every file there and as long as it should be (a cut-short copy is rebuilt)
        for (const auto& [lang, bytes] : produced) {
            std::error_code size_error;
            const uintmax_t size = fs::file_size(root / "Data" / "message" / ("mes_" + lang + ".txtres"), size_error);
            if (size_error || size != bytes.size()) return false;
        }
        return true;
    };
    if (!complete()) {
        fs::path temp = root;
        temp += ".tmp";
        fs::remove_all(temp, error);
        fs::create_directories(temp / "Data" / "message", error);
        bool ok = !error;
        for (const auto& [lang, bytes] : produced) {
            std::ofstream out(temp / "Data" / "message" / ("mes_" + lang + ".txtres"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            out.close();   // the buffered tail is written here: a full disk shows up only now
            ok = ok && !out.fail();
        }
        if (ok) { fs::remove_all(root, error); fs::rename(temp, root, error); ok = !error; }
        if (!ok) {
            fs::remove_all(temp, error);
            for (const auto& [lang, mods_set] : contributors) for (const auto& mod : mods_set) add_problem(mod, "text: the changed text files cannot be written (disk full?)");
            return build;
        }
    }
    build.root = root;
    return build;
}

void prune_text_cache(const fs::path& cache_root, const fs::path& keep) {
    std::error_code error;
    const fs::path dir = cache_root / "_text";
    std::vector<fs::path> remove;
    for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error))
        if (it->path().lexically_normal() != keep.lexically_normal()) remove.push_back(it->path());
    for (const auto& path : remove) fs::remove_all(path, error);
}

}  // namespace uc::mods
