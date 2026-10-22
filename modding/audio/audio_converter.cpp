#include "audio/audio_converter.h"
#include "audio/brstm.h"
#include "audio/convert.h"
#include "audio/decode.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

namespace uc::mods {
namespace fs = std::filesystem;
namespace {
constexpr const char* kConverterVersion = "audio-1";   // part of the cache key: change it when the conversion changes
constexpr uintmax_t kMaxSourceBytes = 512ull * 1024 * 1024;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// File names as text for messages. path::string() throws for names outside the ANSI code page; this does not.
std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool valid_stem(const std::string& stem) {   // the streams on the disc are named with letters, digits, '_' and '-' only
    return !stem.empty() && stem.size() <= 64 && std::all_of(stem.begin(), stem.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
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

std::string read_text(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

struct Source { std::string stem; fs::path path; };

// audio/*.ogg|*.wav of a mod, one per stream name (the first in file name order wins). `ignored` lists the files that cannot be used, with the reason.
std::vector<Source> scan_audio(const fs::path& mod_root, std::vector<std::pair<fs::path, std::string>>& ignored) {
    std::vector<fs::path> files;
    std::error_code error;
    for (fs::directory_iterator it(mod_root / "audio", error), end; !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error) || it->is_symlink(entry_error)) continue;
        const std::string ext = lower(utf8(it->path().extension()));
        if (ext == ".ogg" || ext == ".wav") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    std::vector<Source> sources;
    std::set<std::string> seen;
    for (const auto& path : files) {
        const std::string stem = lower(utf8(path.stem()));
        if (!valid_stem(stem)) { ignored.emplace_back(path, "ignored: a stream name has letters, digits, '_' and '-' only"); continue; }
        if (seen.insert(stem).second) sources.push_back({stem, path});
        else ignored.emplace_back(path, "ignored: another file in audio/ already replaces the stream '" + stem + "'");
    }
    return sources;
}
}  // namespace

AudioConverter::AudioConverter(fs::path cache_root, OriginalLookup lookup, std::function<void()> on_change)
    : cache_root_(std::move(cache_root)), lookup_(std::move(lookup)), on_change_(std::move(on_change)) {
    worker_ = std::thread([this] { run(); });
}

AudioConverter::~AudioConverter() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

fs::path AudioConverter::generated_root(const std::string& mod_id) const { return mod_cache(mod_id) / "generated"; }

void AudioConverter::forget_except(const std::set<std::string>& known_ids) {
    std::error_code error;
    std::vector<fs::path> remove;
    for (fs::directory_iterator it(cache_root_ / "_audio", error), end; !error && it != end; it.increment(error))
        if (!known_ids.count(utf8(it->path().filename()))) remove.push_back(it->path());
    for (const auto& path : remove) fs::remove_all(path, error);
}

void AudioConverter::prune_cache(const std::string& mod_id) {
    std::set<std::string> keep;   // the keys the stamps name (the last word of each stamp)
    std::error_code error;
    for (fs::directory_iterator it(mod_cache(mod_id) / "keys", error), end; !error && it != end; it.increment(error)) {
        const std::string stamp = read_text(it->path());
        keep.insert(stamp.substr(stamp.rfind(' ') + 1) + ".brstm");   // rfind gives npos for a one-word stamp: npos + 1 is 0, the whole text
    }
    std::vector<fs::path> remove;
    for (fs::directory_iterator it(mod_cache(mod_id) / "audio", error), end; !error && it != end; it.increment(error))
        if (!keep.count(utf8(it->path().filename()))) remove.push_back(it->path());
    for (const auto& path : remove) fs::remove(path, error);
}

void AudioConverter::set_active(const std::vector<const ModEntry*>& mods) {
    struct Scan { const ModEntry* mod; std::vector<Source> sources; std::vector<std::pair<fs::path, std::string>> ignored; };
    std::vector<Scan> scans;   // directory scans happen outside the lock
    for (const ModEntry* mod : mods) {
        Scan scan{mod, {}, {}};
        scan.sources = scan_audio(mod->root, scan.ignored);
        scans.push_back(std::move(scan));
    }
    std::lock_guard lock(mutex_);
    std::set<std::string> active;
    for (const ModEntry* mod : mods) active.insert(mod->manifest.id);
    for (auto it = mods_.begin(); it != mods_.end();) {   // mods that are off: their work stops; their files stay (the game may be reading one, the mod may return)
        if (active.count(it->first)) { ++it; continue; }
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Job& j) { return j.mod_id == it->first; }), queue_.end());
        it = mods_.erase(it);
    }
    for (auto& scan : scans) {
        const std::string& id = scan.mod->manifest.id;
        ModState& state = mods_[id];
        ++state.generation;   // work still running for an earlier call of this mod is stale now
        state.total = scan.sources.size();
        state.done = 0;
        state.problems.clear();
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Job& j) { return j.mod_id == id; }), queue_.end());
        for (const auto& [path, why] : scan.ignored) state.problems.push_back("audio: " + utf8(path.filename()) + ": " + why);
        std::set<std::string> stems;
        for (const auto& source : scan.sources) stems.insert(source.stem);
        const fs::path stream_dir = generated_root(id) / "Data" / "sound" / "stream";
        std::error_code error;
        for (fs::directory_iterator it(stream_dir, error), end; !error && it != end; it.increment(error)) {   // generated files whose source is gone
            if (it->path().extension() != ".brstm") continue;
            const std::string stem = lower(utf8(it->path().stem()));
            if (stems.count(stem)) continue;
            if (served_.count(id + "/" + stem)) {   // the game may have it open: it goes at the next start
                state.problems.push_back("audio: " + stem + ".brstm: the source file was removed, the converted stream is replaced at the next start of the game");
                continue;
            }
            std::error_code remove_error;
            fs::remove(it->path(), remove_error);
            fs::remove(mod_cache(id) / "keys" / it->path().stem(), remove_error);
        }
        for (const auto& source : scan.sources) {
            Job job{id, scan.mod->root, source.path, source.stem, std::nullopt, state.generation};
            const auto loop = scan.mod->manifest.audio.find(source.stem);
            if (loop != scan.mod->manifest.audio.end()) job.loop = loop->second;
            queue_.push_back(std::move(job));
        }
    }
    wake_.notify_all();
}

std::string AudioConverter::status(const std::string& mod_id) const {
    std::lock_guard lock(mutex_);
    const auto it = mods_.find(mod_id);
    if (it == mods_.end()) return {};
    const ModState& state = it->second;
    std::string text;
    if (state.done < state.total) text = "converting audio (" + std::to_string(state.done) + " of " + std::to_string(state.total) + ")";
    for (const auto& problem : state.problems) text += (text.empty() ? "" : "; ") + problem;
    return text;
}

void AudioConverter::wait_idle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return queue_.empty() && !busy_; });
}

bool AudioConverter::stale(const Job& job) const {
    std::lock_guard lock(mutex_);
    const auto it = mods_.find(job.mod_id);
    return stop_ || it == mods_.end() || it->second.generation != job.generation;
}

void AudioConverter::add_problem(const Job& job, const std::string& text) {
    std::lock_guard lock(mutex_);
    const auto it = mods_.find(job.mod_id);
    if (it != mods_.end() && it->second.generation == job.generation) it->second.problems.push_back("audio: " + utf8(job.source.filename()) + ": " + text);
}

void AudioConverter::run() {
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
        if (stop_) break;
        const Job job = std::move(queue_.front());
        queue_.pop_front();
        busy_ = true;
        lock.unlock();
        process(job);
        lock.lock();
        const auto it = mods_.find(job.mod_id);
        if (it != mods_.end() && it->second.generation == job.generation) ++it->second.done;
        const bool last_of_mod = std::none_of(queue_.begin(), queue_.end(), [&](const Job& j) { return j.mod_id == job.mod_id; });
        if (last_of_mod && !stop_) {   // every stamp of this mod is written now: what no stamp names is an old version
            lock.unlock();
            prune_cache(job.mod_id);
            lock.lock();
        }
        busy_ = false;
        idle_.notify_all();
    }
    idle_.notify_all();
}

void AudioConverter::process(const Job& job) {
    try {
        std::error_code error;
        const uintmax_t size = fs::file_size(job.source, error);
        if (error) return add_problem(job, "cannot read the file");
        if (size > kMaxSourceBytes) return add_problem(job, "the file is larger than 512 MB");
        if (fs::exists(job.mod_root / "files" / "Data" / "sound" / "stream" / (job.stem + ".brstm"), error))   // the mod's own ready stream wins in the overlay: converting would be wasted
            return add_problem(job, "ignored: the mod's own files/Data/sound/stream/" + job.stem + ".brstm is used instead");
        std::string why;
        const std::optional<StreamOriginal> original = lookup_(job.stem, why);
        if (!original) return add_problem(job, (why.empty() ? "the game has no usable stream named '" + job.stem + "'" : why) + "; the file is ignored");
        const fs::path generated = generated_root(job.mod_id) / "Data" / "sound" / "stream" / (job.stem + ".brstm");
        const fs::path stamp = mod_cache(job.mod_id) / "keys" / job.stem;   // outside the generated root: that folder is laid over the disc as it is
        const std::string served_key = job.mod_id + "/" + job.stem;

        // The stamp holds "<size> <time> <parameters> <key>": size and time of the source and the target (rate, channels, loop, converter version) are the quick
        // check, so an apply that finds all three unchanged does not read the file again.
        const uint32_t parameters[] = {original->rate, original->channels, job.loop ? job.loop->loop_start : 0xFFFFFFFFu, job.loop ? job.loop->loop_end : 0xFFFFFFFFu};
        const uint64_t parameter_hash = fnv1a(fnv1a(1469598103934665603ull, parameters, sizeof parameters), kConverterVersion, std::char_traits<char>::length(kConverterVersion));
        const auto written = fs::last_write_time(job.source, error);
        const std::string source_stamp = std::to_string(size) + " " + std::to_string(error ? 0 : int64_t(written.time_since_epoch().count())) + " " + hex(parameter_hash);
        const std::string old_stamp = read_text(stamp);
        const bool published_before = fs::exists(generated, error);
        if (published_before && old_stamp.rfind(source_stamp + " ", 0) == 0) {
            std::lock_guard lock(mutex_);
            served_.insert(job.mod_id + "/" + job.stem);
            return;
        }

        std::vector<uint8_t> bytes;   // read in pieces: a long read can be abandoned (switch-off, exit)
        bytes.reserve(size_t(size));
        uint64_t hash = 1469598103934665603ull;
        {
            std::ifstream in(job.source, std::ios::binary);
            std::vector<uint8_t> piece(4 << 20);
            while (in) {
                in.read(reinterpret_cast<char*>(piece.data()), std::streamsize(piece.size()));
                const size_t got = size_t(in.gcount());
                if (got == 0) break;
                hash = fnv1a(hash, piece.data(), got);
                bytes.insert(bytes.end(), piece.begin(), piece.begin() + std::streamsize(got));
                if (stale(job)) return;
            }
        }
        if (bytes.size() != size) return add_problem(job, "cannot read the file");
        hash = fnv1a(hash, &parameter_hash, sizeof parameter_hash);
        const std::string key = hex(hash);
        if (stale(job)) return;

        bool serving = false;
        {
            std::lock_guard lock(mutex_);
            serving = served_.count(served_key) != 0;
        }
        const bool published = published_before;
        if (published && old_stamp.substr(old_stamp.rfind(' ') + 1) == key) {   // the same content and parameters as the published file: only the file's time changed
            std::ofstream(stamp, std::ios::binary | std::ios::trunc) << source_stamp << ' ' << key;
            std::lock_guard lock(mutex_);
            served_.insert(served_key);
            return;
        }
        if (published && serving) return add_problem(job, "the file changed: the game may be playing the old converted stream, the new one is used at the next start of the game");

        const fs::path cached = mod_cache(job.mod_id) / "audio" / (key + ".brstm");
        if (!fs::exists(cached, error)) {
            DecodeResult decoded;
            if (lower(utf8(job.source.extension())) == ".ogg") {
                DecodeLimits limits;
                limits.max_seconds = kMaxSourceSeconds;
                limits.cancel = [&] { return stale(job); };
                decoded = read_ogg(bytes, limits);
                if (!decoded.audio && decoded.error == "cancelled") return;
            } else {
                decoded = read_wav(bytes);
            }
            if (!decoded.audio) return add_problem(job, decoded.error);
            bytes = {};   // the decoded samples are all that is needed now
            ConvertOptions options;
            options.target_rate = original->rate;
            options.target_channels = original->channels;
            if (job.loop) options.loop_override = {{job.loop->loop_start, job.loop->loop_end}};
            options.cancel = [&] { return stale(job); };
            ConvertResult converted = convert_audio(std::move(*decoded.audio), options);
            if (!converted.audio) {
                if (converted.error == "cancelled") return;
                return add_problem(job, converted.error);
            }
            for (const auto& note : converted.notes) {
                std::fprintf(stderr, "[mods] audio %s: %s: %s\n", job.mod_id.c_str(), utf8(job.source.filename()).c_str(), note.c_str());
                if (note.find("ignored") != std::string::npos) add_problem(job, note);   // a dropped loop is something the author must fix
            }
            if (stale(job)) return;
            const std::vector<uint8_t> brstm = write_brstm_pcm16(*converted.audio);
            fs::create_directories(cached.parent_path());
            fs::path temp = cached;
            temp += ".tmp";   // never published under the final name before it is complete
            {
                std::ofstream out(temp, std::ios::binary);
                out.write(reinterpret_cast<const char*>(brstm.data()), std::streamsize(brstm.size()));
                if (!out.good()) { out.close(); fs::remove(temp, error); return add_problem(job, "cannot write the converted file (disk full?)"); }
            }
            fs::rename(temp, cached, error);
            if (error) { fs::remove(temp, error); return add_problem(job, "cannot store the converted file: " + error.message()); }
            ++conversions_;
        }
        if (stale(job)) return;
        fs::create_directories(generated.parent_path());
        fs::create_directories(stamp.parent_path());
        fs::path temp = generated;
        temp += ".tmp";
        fs::copy_file(cached, temp, fs::copy_options::overwrite_existing, error);
        if (error) return add_problem(job, "cannot publish the converted file: " + error.message());
        for (int attempt = 0; attempt < 5; ++attempt) {   // a read of the game's DVD layer can hold the name for a moment
            fs::rename(temp, generated, error);
            if (!error || stale(job)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (error) { std::error_code ignore; fs::remove(temp, ignore); return add_problem(job, "cannot replace the converted file (is the game using it?): " + error.message()); }
        std::ofstream(stamp, std::ios::binary | std::ios::trunc) << source_stamp << ' ' << key;
        {
            std::lock_guard lock(mutex_);
            served_.insert(served_key);
        }
        if (on_change_) on_change_();
    } catch (const std::exception& e) {
        add_problem(job, std::string("conversion failed: ") + e.what());
    }
}

}  // namespace uc::mods
