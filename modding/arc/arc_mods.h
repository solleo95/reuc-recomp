#pragma once
// Archive members for mods: <mod>/files/<disc path>.arc!/<path inside> replaces or adds one member of that U8 archive (docs/modding/archives.md). The active
// mods' members are applied in load order to the archive (the disc's file, or the whole archive of a later mod) and the result is written to a generated
// overlay root. Synchronous: an archive is a few megabytes and the output is cached.
#include "registry.h"
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace uc::mods {

struct ArcBuild {
    std::filesystem::path root;                                       // <cache>/_arc/<key>; empty: no archive to overlay
    std::map<std::string, std::vector<std::string>> problems;         // by mod id: "arc: <what>" lines (at most 20 per mod, then a note)
    std::vector<std::string> conflicts;                               // "archive member <arc>!/<path> is set by A and B (the last one wins)"
};

// The archive file of the disc by its path below DATA/files ("Data/chara/EZ01.arc"); nullopt: the game has none.
using ArcBase = std::function<std::optional<std::vector<uint8_t>>(const std::string& disc_path)>;

// The last build, for callers that build again on every change of the mod list: while the inputs (every member file and whole archive of the active mods,
// in load order, with size and modification time) are the same and the generated folder is still there, the result is returned without reading the
// archives again.
struct ArcMemo {
    std::string signature;
    ArcBuild result;
};

// The files are written under <cache_root>/_arc/<key>/<archive path> (key = hash of the produced bytes); a folder of the same key is reused if every file is
// there at its length, a different content gets a new folder, so an archive the game has open is never rewritten.
ArcBuild build_arc_mods(const std::filesystem::path& cache_root, const std::vector<const ModEntry*>& mods, const ArcBase& base, ArcMemo* memo = nullptr);

// Deletes the other folders under <cache_root>/_arc (and half-written ones); call it when no file of the cache can be open (at start).
void prune_arc_cache(const std::filesystem::path& cache_root, const std::filesystem::path& keep);

}  // namespace uc::mods
