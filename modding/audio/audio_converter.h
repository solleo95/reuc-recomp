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
#include <string>
#include <thread>
#include <vector>

namespace uc::mods {

struct StreamOriginal { uint32_t rate = 0; uint32_t channels = 0; };   // from the disc's BRSTM header
using OriginalLookup = std::function<std::optional<StreamOriginal>(const std::string& stem)>;   // nullopt: no such stream on the disc

class AudioConverter {
public:
    // cache_root: <mods>/.cache; on_change runs on the worker after a generated file appeared, changed or was removed (the caller rebuilds the overlay).
    AudioConverter(std::filesystem::path cache_root, OriginalLookup lookup, std::function<void()> on_change);
    ~AudioConverter();   // stops the worker (a running conversion is abandoned at its next poll) and joins
    AudioConverter(const AudioConverter&) = delete;
    AudioConverter& operator=(const AudioConverter&) = delete;

    // The mods whose audio should be converted from now on. Work for mods no longer listed is dropped and their generated files are removed.
    void set_active(const std::vector<const ModEntry*>& mods);
    std::filesystem::path generated_root(const std::string& mod_id) const;   // <cache_root>/<id>/generated
    // "" when idle and fine; "converting audio (2 of 5)"; error lines "audio: <file>: <reason>" joined with "; " (also while converting).
    std::string status(const std::string& mod_id) const;
    void wait_idle();   // blocks until the queue is empty and nothing is converting (tests, shutdown)
    size_t conversions() const { return conversions_.load(); }   // files actually converted (cache hits do not count)

private:
    struct Job { std::string mod_id; std::filesystem::path source; std::string stem; std::optional<AudioLoop> loop; uint64_t generation = 0; };
    struct ModState { uint64_t generation = 0; size_t total = 0, done = 0; std::vector<std::string> problems; };

    void run();
    void process(const Job& job);
    bool stale(const Job& job) const;
    void add_problem(const Job& job, const std::string& text);

    std::filesystem::path cache_root_;
    OriginalLookup lookup_;
    std::function<void()> on_change_;
    mutable std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::deque<Job> queue_;
    std::map<std::string, ModState> mods_;
    bool busy_ = false, stop_ = false;
    std::atomic<size_t> conversions_{0};
    std::thread worker_;   // last: starts after everything above exists
};

}  // namespace uc::mods
