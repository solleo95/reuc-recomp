#include "check.h"
#include "audio/audio_converter.h"
#include "audio/brstm.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>

using namespace uc::mods;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {
constexpr double kPi = 3.14159265358979323846;

fs::path temp_dir(const char* name) {
    const fs::path p = fs::temp_directory_path() / (std::string("uc_audio_conv_") + name + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void le16(std::vector<uint8_t>& d, uint32_t v) { d.push_back(uint8_t(v)); d.push_back(uint8_t(v >> 8)); }
void le32(std::vector<uint8_t>& d, uint32_t v) { for (int i = 0; i < 4; ++i) d.push_back(uint8_t(v >> (8 * i))); }

// A mono 16-bit WAV of a sine (or silence), optionally with a smpl loop (start, last sample).
std::vector<uint8_t> wav(uint32_t rate, double seconds, double frequency, std::optional<std::pair<uint32_t, uint32_t>> loop = {}) {
    std::vector<uint8_t> d;
    const uint32_t frames = uint32_t(std::llround(rate * seconds));
    d.insert(d.end(), {'R', 'I', 'F', 'F'}); le32(d, 0); d.insert(d.end(), {'W', 'A', 'V', 'E'});
    d.insert(d.end(), {'f', 'm', 't', ' '}); le32(d, 16); le16(d, 1); le16(d, 1); le32(d, rate); le32(d, rate * 2); le16(d, 2); le16(d, 16);
    if (loop) {
        d.insert(d.end(), {'s', 'm', 'p', 'l'}); le32(d, 60);
        for (int i = 0; i < 7; ++i) le32(d, 0);
        le32(d, 1); le32(d, 0); le32(d, 0); le32(d, 0); le32(d, loop->first); le32(d, loop->second); le32(d, 0); le32(d, 0);
    }
    d.insert(d.end(), {'d', 'a', 't', 'a'}); le32(d, frames * 2);
    for (uint32_t i = 0; i < frames; ++i) le16(d, frequency == 0 ? 0 : uint32_t(uint16_t(int16_t(std::lround(8000 * std::sin(2 * kPi * frequency * i / rate))))));
    const uint32_t riff = uint32_t(d.size() - 8);
    for (int i = 0; i < 4; ++i) d[4 + size_t(i)] = uint8_t(riff >> (8 * i));
    return d;
}

void write_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

std::vector<uint8_t> read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

ModEntry make_mod(const fs::path& root, const std::string& id) {
    ModEntry m;
    m.manifest.id = id;
    m.root = root;
    m.state = ModState::enabled;
    return m;
}

struct Rig {
    fs::path base = temp_dir("rig");
    fs::path cache = base / "cache", mod_root = base / "mod";
    std::mutex mutex;
    std::condition_variable cv;
    int changes = 0;
    std::map<std::string, StreamOriginal> originals{{"tone", {32000, 1}}, {"good", {32000, 1}}, {"title", {48000, 2}}, {"loopy", {32000, 1}}, {"long", {32000, 1}}};
    std::unique_ptr<AudioConverter> make() {
        return std::make_unique<AudioConverter>(cache, [this](const std::string& stem) -> std::optional<StreamOriginal> {
            const auto it = originals.find(stem);
            return it == originals.end() ? std::nullopt : std::optional<StreamOriginal>(it->second);
        }, [this] { { std::lock_guard lock(mutex); ++changes; } cv.notify_all(); });
    }
    bool wait_changes(int n) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 20s, [&] { return changes >= n; });
    }
    fs::path generated(const AudioConverter& c, const char* stem) const { return c.generated_root("mymod") / "Data" / "sound" / "stream" / (std::string(stem) + ".brstm"); }
};
}  // namespace

static void converts_and_publishes_generated_file() {
    Rig rig;
    write_file(rig.mod_root / "audio" / "Tone.wav", wav(44100, 0.5, 440));
    const ModEntry mod = make_mod(rig.mod_root, "mymod");
    auto converter = rig.make();
    converter->set_active({&mod});
    CHECK(rig.wait_changes(1));
    converter->wait_idle();
    const auto file = read_file(rig.generated(*converter, "tone"));
    const auto audio = read_brstm(file);
    CHECK(audio && audio->rate == 32000 && audio->channels.size() == 1 && std::abs(int(audio->frames()) - 16000) <= 1);
    CHECK(converter->status("mymod").empty() && converter->conversions() == 1);
}

static void cache_hit_does_not_reconvert() {
    Rig rig;
    write_file(rig.mod_root / "audio" / "tone.wav", wav(32000, 0.3, 440));
    const ModEntry mod = make_mod(rig.mod_root, "mymod");
    {
        auto first = rig.make();
        first->set_active({&mod});
        CHECK(rig.wait_changes(1));
        first->wait_idle();
        CHECK(first->conversions() == 1);
    }
    fs::remove_all(rig.cache / "mymod" / "generated");   // as after a restart that lost the generated tree but kept the cache
    auto second = rig.make();
    second->set_active({&mod});
    CHECK(rig.wait_changes(2));
    second->wait_idle();
    CHECK(second->conversions() == 0 && fs::exists(rig.generated(*second, "tone")));   // published from the cache
    // unchanged generated tree: nothing to do and no change notification
    const int before = rig.changes;
    second->set_active({&mod});
    second->wait_idle();
    CHECK(rig.changes == before && second->conversions() == 0);
}

static void changed_source_is_reconverted() {
    Rig rig;
    write_file(rig.mod_root / "audio" / "tone.wav", wav(32000, 0.3, 440));
    const ModEntry mod = make_mod(rig.mod_root, "mymod");
    auto converter = rig.make();
    converter->set_active({&mod});
    CHECK(rig.wait_changes(1));
    converter->wait_idle();
    const auto first = read_file(rig.generated(*converter, "tone"));
    write_file(rig.mod_root / "audio" / "tone.wav", wav(32000, 0.4, 523));
    converter->set_active({&mod});
    CHECK(rig.wait_changes(2));
    converter->wait_idle();
    const auto second = read_file(rig.generated(*converter, "tone"));
    CHECK(first != second && converter->conversions() == 2);
}

static void unknown_stream_is_reported_and_ignored() {
    Rig rig;
    write_file(rig.mod_root / "audio" / "nosuchstream.ogg", wav(32000, 0.2, 440));   // the name matches no disc stream
    const ModEntry mod = make_mod(rig.mod_root, "mymod");
    auto converter = rig.make();
    converter->set_active({&mod});
    converter->wait_idle();
    CHECK(converter->status("mymod").find("nosuchstream") != std::string::npos && converter->status("mymod").find("no stream") != std::string::npos);
    CHECK(!fs::exists(rig.generated(*converter, "nosuchstream")) && converter->conversions() == 0);
}

static void bad_file_reports_error_and_others_still_convert() {
    Rig rig;
    write_file(rig.mod_root / "audio" / "tone.wav", std::vector<uint8_t>(100, 0x42));   // not a WAV
    write_file(rig.mod_root / "audio" / "good.wav", wav(32000, 0.2, 440));
    const ModEntry mod = make_mod(rig.mod_root, "mymod");
    auto converter = rig.make();
    converter->set_active({&mod});
    CHECK(rig.wait_changes(1));
    converter->wait_idle();
    const std::string status = converter->status("mymod");
    CHECK(status.find("tone.wav") != std::string::npos && status.rfind("audio:", 0) == 0);
    CHECK(fs::exists(rig.generated(*converter, "good")) && !fs::exists(rig.generated(*converter, "tone")));
}

static void switching_a_mod_off_removes_its_generated_files() {
    Rig rig;
    write_file(rig.mod_root / "audio" / "tone.wav", wav(32000, 0.2, 440));
    const ModEntry mod = make_mod(rig.mod_root, "mymod");
    auto converter = rig.make();
    converter->set_active({&mod});
    CHECK(rig.wait_changes(1));
    converter->wait_idle();
    CHECK(fs::exists(rig.generated(*converter, "tone")));
    converter->set_active({});
    converter->wait_idle();
    CHECK(!fs::exists(converter->generated_root("mymod")) && converter->status("mymod").empty());
    // a removed source file takes its generated file with it
    converter->set_active({&mod});
    CHECK(rig.wait_changes(2));
    converter->wait_idle();
    fs::remove(rig.mod_root / "audio" / "tone.wav");
    converter->set_active({&mod});
    converter->wait_idle();
    CHECK(!fs::exists(rig.generated(*converter, "tone")));
}

static void destructor_stops_a_long_conversion_and_publishes_nothing_half_written() {
    Rig rig;
    write_file(rig.mod_root / "audio" / "long.wav", wav(8000, 1500, 0));   // 25 minutes of silence at 8 kHz: a long resample to 32 kHz
    const ModEntry mod = make_mod(rig.mod_root, "mymod");
    const auto start = std::chrono::steady_clock::now();
    {
        auto converter = rig.make();
        converter->set_active({&mod});
        std::this_thread::sleep_for(300ms);   // let the worker start
    }
    CHECK(std::chrono::steady_clock::now() - start < 5s);
    size_t brstm = 0;
    if (fs::exists(rig.cache)) for (const auto& e : fs::recursive_directory_iterator(rig.cache)) brstm += e.is_regular_file() && e.path().extension() == ".brstm";
    CHECK(brstm == 0);   // abandoned work leaves no result, finished or half written
}

static void manifest_loop_wins_over_file_loop() {
    Rig rig;
    write_file(rig.mod_root / "audio" / "loopy.wav", wav(32000, 1.0, 440, std::make_pair(100u, 199u)));   // the file's loop: 100 to 200
    ModEntry mod = make_mod(rig.mod_root, "mymod");
    mod.manifest.audio["loopy"] = AudioLoop{8000, 24000};
    auto converter = rig.make();
    converter->set_active({&mod});
    CHECK(rig.wait_changes(1));
    converter->wait_idle();
    const auto audio = read_brstm(read_file(rig.generated(*converter, "loopy")));
    CHECK(audio && audio->loop && audio->loop->first == 8000u && audio->loop->second == 24000u && audio->frames() == 24000);
}

static void file_loop_is_used_without_a_manifest_entry() {
    Rig rig;
    write_file(rig.mod_root / "audio" / "loopy.wav", wav(32000, 1.0, 440, std::make_pair(100u, 19999u)));
    const ModEntry mod = make_mod(rig.mod_root, "mymod");
    auto converter = rig.make();
    converter->set_active({&mod});
    CHECK(rig.wait_changes(1));
    converter->wait_idle();
    const auto audio = read_brstm(read_file(rig.generated(*converter, "loopy")));
    CHECK(audio && audio->loop && audio->loop->first == 100u && audio->loop->second == 20000u);
}

static void stereo_original_gets_stereo() {
    Rig rig;
    write_file(rig.mod_root / "audio" / "title.wav", wav(44100, 0.3, 440));   // mono file for a stereo 48 kHz stream
    const ModEntry mod = make_mod(rig.mod_root, "mymod");
    auto converter = rig.make();
    converter->set_active({&mod});
    CHECK(rig.wait_changes(1));
    converter->wait_idle();
    const auto audio = read_brstm(read_file(rig.generated(*converter, "title")));
    CHECK(audio && audio->rate == 48000 && audio->channels.size() == 2 && audio->channels[0] == audio->channels[1]);
}

int main() {
    converts_and_publishes_generated_file();
    cache_hit_does_not_reconvert();
    changed_source_is_reconverted();
    unknown_stream_is_reported_and_ignored();
    bad_file_reports_error_and_others_still_convert();
    switching_a_mod_off_removes_its_generated_files();
    destructor_stops_a_long_conversion_and_publishes_nothing_half_written();
    manifest_loop_wins_over_file_loop();
    file_loop_is_used_without_a_manifest_entry();
    stereo_original_gets_stereo();
    std::puts("test_audio_converter: ok");
    return 0;
}
