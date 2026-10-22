#include "audio/audio_converter.h"
#include "audio/brstm.h"
#include "audio/convert.h"
#include "audio/decode.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
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

// audio/*.ogg|*.wav of a mod, one per stream name (the first in file name order wins); `ignored` lists the files that lost.
std::vector<Source> scan_audio(const fs::path& mod_root, std::vector<std::pair<fs::path, std::string>>& ignored) {
    std::vector<fs::path> files;
    std::error_code error;
    for (fs::directory_iterator it(mod_root / "audio", error), end; !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error) || it->is_symlink(entry_error)) continue;
        const std::string ext = lower(it->path().extension().string());
        if (ext == ".ogg" || ext == ".wav") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    std::vector<Source> sources;
    std::set<std::string> seen;
    for (const auto& path : files) {
        const std::string stem = lower(path.stem().string());
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

fs::path AudioConverter::generated_root(const std::string& mod_id) const { return cache_root_ / mod_id / "generated"; }

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
    for (auto it = mods_.begin(); it != mods_.end();) {   // mods that are off: their work and their generated files go
        if (active.count(it->first)) { ++it; continue; }
        std::error_code error;
        fs::remove_all(generated_root(it->first), error);
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
        for (const auto& [path, why] : scan.ignored) state.problems.push_back("audio: " + path.filename().string() + ": " + why);
        std::set<std::string> stems;
        for (const auto& source : scan.sources) stems.insert(source.stem);
        const fs::path stream_dir = generated_root(id) / "Data" / "sound" / "stream";
        std::error_code error;
        for (fs::directory_iterator it(stream_dir, error), end; !error && it != end; it.increment(error)) {   // generated files whose source is gone
            if (it->path().extension() != ".brstm" || stems.count(lower(it->path().stem().string()))) continue;
            std::error_code remove_error;
            fs::remove(it->path(), remove_error);
            fs::remove(generated_root(id) / ".keys" / it->path().stem(), remove_error);
        }
        for (const auto& source : scan.sources) {
            Job job{id, source.path, source.stem, std::nullopt, state.generation};
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
    if (it != mods_.end() && it->second.generation == job.generation) it->second.problems.push_back("audio: " + job.source.filename().string() + ": " + text);
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
        busy_ = false;
        const auto it = mods_.find(job.mod_id);
        if (it != mods_.end() && it->second.generation == job.generation) ++it->second.done;
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
        const std::optional<StreamOriginal> original = lookup_(job.stem);
        const fs::path generated = generated_root(job.mod_id) / "Data" / "sound" / "stream" / (job.stem + ".brstm");
        const fs::path stamp = generated_root(job.mod_id) / ".keys" / job.stem;
        if (!original) {
            fs::remove(generated, error);
            return add_problem(job, "the game has no stream named '" + job.stem + "' (sound/stream/" + job.stem + ".brstm), the file is ignored");
        }
        std::ifstream in(job.source, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
        if (bytes.size() != size) return add_problem(job, "cannot read the file");

        uint64_t hash = fnv1a(1469598103934665603ull, bytes.data(), bytes.size());
        const uint32_t parameters[] = {original->rate, original->channels, job.loop ? job.loop->loop_start : 0xFFFFFFFFu, job.loop ? job.loop->loop_end : 0xFFFFFFFFu};
        hash = fnv1a(hash, parameters, sizeof parameters);
        hash = fnv1a(hash, kConverterVersion, std::char_traits<char>::length(kConverterVersion));
        const std::string key = hex(hash);

        if (fs::exists(generated, error) && read_text(stamp) == key) return;   // already published for exactly this source and these parameters

        const fs::path cached = cache_root_ / job.mod_id / "audio" / (key + ".brstm");
        if (!fs::exists(cached, error)) {
            const std::string ext = lower(job.source.extension().string());
            DecodeResult decoded = ext == ".ogg" ? read_ogg(bytes) : read_wav(bytes);
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
            for (const auto& note : converted.notes)
                if (note.find("ignored") != std::string::npos) add_problem(job, note);
            if (stale(job)) return;
            const std::vector<uint8_t> brstm = write_brstm_pcm16(*converted.audio);
            fs::create_directories(cached.parent_path());
            const fs::path temp = cached.string() + ".tmp";   // never published under the final name before it is complete
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
        const fs::path temp = generated.string() + ".tmp";
        fs::copy_file(cached, temp, fs::copy_options::overwrite_existing, error);
        if (error) return add_problem(job, "cannot publish the converted file: " + error.message());
        fs::rename(temp, generated, error);
        if (error) { std::error_code ignore; fs::remove(temp, ignore); return add_problem(job, "cannot replace the converted file (is the game using it?): " + error.message()); }
        std::ofstream(stamp, std::ios::binary | std::ios::trunc) << key;
        if (on_change_) on_change_();
    } catch (const std::exception& e) {
        add_problem(job, std::string("conversion failed: ") + e.what());
    }
}

}  // namespace uc::mods
