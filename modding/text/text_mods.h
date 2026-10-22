#pragma once
// Text replacement for mods: text/<lang>.json and text/all.json of the active mods, applied in load order to the game's mes_<lang>.txtres files and
// written to a generated overlay root (docs/modding/text.md). Synchronous: the files are a quarter of a megabyte each.
#include "registry.h"
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace uc::mods {

inline constexpr const char* kTextLanguages[] = {"us", "en", "fr", "ge", "it", "jp", "sp", "du"};

struct TextBuild {
    std::filesystem::path root;                                       // <cache>/_text/<key>; empty: no text to overlay
    std::map<std::string, std::vector<std::string>> problems;         // by mod id: "text: <file>: <what>" lines (at most 20 per mod, then a note)
    std::vector<std::string> conflicts;                               // "text id N is set by A and B (the last one wins)": two mods changed one record
};

struct TextBaseFile {
    std::vector<uint8_t> bytes;
    std::string from_mod;   // the mod that ships this file under files/; empty: the game's own file
};
// The unpatched file of a language; nullopt: the game has none.
using TextBase = std::function<std::optional<TextBaseFile>(const std::string& lang)>;

// The files are written under <cache_root>/_text/<key>/Data/message/mes_<lang>.txtres (key = hash of the produced bytes); a folder of the same key is
// reused, a different content gets a new folder, so a file the game has open is never rewritten.
TextBuild build_text_mods(const std::filesystem::path& cache_root, const std::vector<const ModEntry*>& mods, const TextBase& base);

// Deletes the other folders under <cache_root>/_text (and half-written ones); call it when no file of the cache can be open (at start).
void prune_text_cache(const std::filesystem::path& cache_root, const std::filesystem::path& keep);

}  // namespace uc::mods
