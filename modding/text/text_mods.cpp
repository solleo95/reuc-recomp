#include "text/text_mods.h"
#include "text/txtres.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <set>
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

struct Parsed {
    std::string file;   // "us.json"
    TextChanges changes;
};

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
    for (const auto& [key, value] : json.items()) {
        const std::string id_text = "id \"" + key + "\"";
        const bool digits = !key.empty() && key.size() <= 10 && std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        if (!digits || std::stoull(key) > 0xFFFFFFFFull) { problem(id_text + " is not a record id (a whole number)"); continue; }
        if (!value.is_string()) { problem(id_text + " must be text"); continue; }
        const std::string& s = value.get_ref<const std::string&>();
        if (s.find('\0') != std::string::npos) { problem(id_text + ": the text holds a NUL character"); continue; }
        std::vector<std::string> lines;
        size_t start = 0;
        for (size_t at; (at = s.find('\n', start)) != std::string::npos; start = at + 1) lines.push_back(s.substr(start, at - start));
        lines.push_back(s.substr(start));
        if (lines.size() > kMaxLines) { problem(id_text + ": more than " + std::to_string(kMaxLines) + " lines"); continue; }
        if (std::any_of(lines.begin(), lines.end(), [](const std::string& l) { return l.size() > kMaxLineBytes; })) { problem(id_text + ": a line is longer than " + std::to_string(kMaxLineBytes) + " bytes"); continue; }
        parsed.changes[uint32_t(std::stoull(key))] = std::move(lines);
    }
    return parsed;
}

struct Origin { std::string mod, file; };
}  // namespace

TextBuild build_text_mods(const fs::path& cache_root, const std::vector<const ModEntry*>& mods, const TextBase& base) {
    TextBuild build;
    std::map<std::string, TextChanges> changes;                    // language -> merged changes of all mods
    std::map<std::string, std::map<uint32_t, Origin>> origin;      // language -> id -> who set it last
    std::map<std::string, std::set<std::string>> contributors;     // language -> mods
    std::map<std::string, std::set<std::string>> seen_problems;    // mod -> problems already listed (the same text is not listed twice)
    const auto add_problem = [&](const std::string& mod, const std::string& text) { if (seen_problems[mod].insert(text).second) build.problems[mod].push_back(text); };
    const auto add_problems = [&](const std::string& mod, const std::vector<std::string>& texts) { for (const auto& t : texts) add_problem(mod, t); };

    std::map<std::string, std::optional<std::vector<uint8_t>>> bases;   // looked up once per language
    const auto base_of = [&](const std::string& lang) -> const std::optional<std::vector<uint8_t>>& {
        auto it = bases.find(lang);
        if (it == bases.end()) it = bases.emplace(lang, base(lang)).first;
        return it->second;
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
            if (stem != "all" && !is_language(stem)) { problems.push_back("text: " + utf8(path.filename()) + ": '" + stem + "' is not a language (us, en, fr, ge, it, jp, sp, du) or 'all'"); continue; }
            Parsed parsed = parse_file(path, problems);
            if (stem == "all") all = std::move(parsed); else by_language[stem] = std::move(parsed);
        }
        add_problems(id, problems);
        const auto apply = [&](const std::string& lang, const Parsed& parsed) {
            for (const auto& [record, lines] : parsed.changes) { changes[lang][record] = lines; origin[lang][record] = {id, parsed.file}; }
            if (!parsed.changes.empty()) contributors[lang].insert(id);
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
    for (auto& [lang, merged] : changes) {
        std::string error;
        const auto patch = patch_txtres(*base_of(lang), merged, error);
        if (!patch) {
            for (const auto& mod : contributors[lang]) add_problem(mod, "text: mes_" + lang + ".txtres cannot be changed: " + error);
            continue;
        }
        for (uint32_t unknown : patch->unknown_ids) {
            const Origin& from = origin[lang][unknown];
            if (lower(from.file) == "all.json") ++all_unknown[{from.mod, unknown}];
            else add_problem(from.mod, "text: " + from.file + ": id " + std::to_string(unknown) + " does not exist in mes_" + lang + ".txtres");
        }
        if (patch->unknown_ids.size() == merged.size()) continue;   // nothing of it applies: no file
        produced[lang] = patch->file;
    }
    for (const auto& [key, languages] : all_unknown)   // an id of all.json is only a problem when no language has it
        if (languages == changes.size()) add_problem(key.first, "text: all.json: id " + std::to_string(key.second) + " does not exist in any language");
    if (produced.empty()) return build;

    uint64_t hash = 1469598103934665603ull;
    for (const auto& [lang, bytes] : produced) { hash = fnv1a(hash, lang.data(), lang.size()); hash = fnv1a(hash, bytes.data(), bytes.size()); }
    const fs::path root = cache_root / "_text" / hex(hash);
    std::error_code error;
    if (!fs::is_directory(root / "Data" / "message", error)) {
        fs::path temp = root;
        temp += ".tmp";
        fs::remove_all(temp, error);
        fs::create_directories(temp / "Data" / "message", error);
        bool ok = !error;
        for (const auto& [lang, bytes] : produced) {
            std::ofstream out(temp / "Data" / "message" / ("mes_" + lang + ".txtres"), std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            ok = ok && out.good();
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
