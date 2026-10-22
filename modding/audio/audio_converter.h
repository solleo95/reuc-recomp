#pragma once
// Converts the audio/<stream>.ogg|.wav files of the active mods into PCM16 BRSTM files on one worker thread, caches the results and keeps, per mod, a
// "generated" overlay root with the disc layout (Data/sound/stream/<stem>.brstm) that the file overlay puts in front of the mod's own files/.
#include "registry.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace uc::mods {

struct StreamOriginal { uint32_t rate = 0; uint32_t channels = 0; };   // from the disc's BRSTM header
// nullopt: no usable stream of that name on the disc; `why` then says what is wrong (no such stream, a layout that cannot be matched).
using OriginalLookup = std::function<std::optional<StreamOriginal>(const std::string& stem, std::string& why)>;

class AudioConverter {
public:
    // cache_root: <mods>/.cache (this class keeps to <cache_root>/_audio/<mod id>/); on_change runs on the worker after a generated file appeared, changed or was removed (the caller rebuilds the overlay).
    AudioConverter(std::filesystem::path cache_root, OriginalLookup lookup, std::function<void()> on_change);
    ~AudioConverter();   // stops the worker (a running conversion is abandoned at its next poll) and joins
    AudioConverter(const AudioConverter&) = delete;
    AudioConverter& operator=(const AudioConverter&) = delete;

    // The mods whose audio should be converted from now on. Work for mods no longer listed is dropped; their generated files stay on disk (a stream the
    // game opened keeps reading its file, and the mod may come back). A generated file the game may already have opened this session is never replaced or
    // removed in place: the change waits for the next start, and the mod's status says so.
    void set_active(const std::vector<const ModEntry*>& mods);
    std::filesystem::path generated_root(const std::string& mod_id) const;   // <cache_root>/_audio/<id>/generated
    // Deletes the cache of every mod that is not in `known_ids` (mods that were removed). Call it at start, before the game opens any file.
    void forget_except(const std::set<std::string>& known_ids);
    // "" when idle and fine; "converting audio (2 of 5)"; error lines "audio: <file>: <reason>" joined with "; " (also while converting).
    std::string status(const std::string& mod_id) const;
    void wait_idle();   // blocks until the queue is empty and nothing is converting (tests, shutdown)
    size_t conversions() const { return conversions_.load(); }   // files actually converted (cache hits do not count)

private:
    struct Job { std::string mod_id; std::filesystem::path mod_root, source; std::string stem; std::optional<AudioLoop> loop; uint64_t generation = 0; };
    struct ModState { uint64_t generation = 0; size_t total = 0, done = 0; std::vector<std::string> problems; };

    void run();
    void process(const Job& job);
    bool stale(const Job& job) const;
    void add_problem(const Job& job, const std::string& text);
    void prune_cache(const std::string& mod_id);   // converted files that no stamp names any more (old versions of an edited source)
    std::filesystem::path mod_cache(const std::string& mod_id) const { return cache_root_ / "_audio" / mod_id; }

    std::filesystem::path cache_root_;
    OriginalLookup lookup_;
    std::function<void()> on_change_;
    mutable std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::deque<Job> queue_;
    std::map<std::string, ModState> mods_;
    std::set<std::string> served_;   // "<mod id>/<stem>": generated files that were put in front of the game this session
    bool busy_ = false, stop_ = false;
    std::atomic<size_t> conversions_{0};
    std::thread worker_;   // last: starts after everything above exists
};

}  // namespace uc::mods
