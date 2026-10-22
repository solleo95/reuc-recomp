#include "check.h"
#include "audio/convert.h"
#include <algorithm>
#include <cmath>

using namespace uc::mods;

namespace {
constexpr double kPi = 3.14159265358979323846;

PcmAudio sine(uint32_t rate, double seconds, double frequency, int channels = 1, double amplitude = 10000) {
    PcmAudio a;
    a.rate = rate;
    const size_t frames = size_t(std::llround(rate * seconds));
    for (int c = 0; c < channels; ++c) {
        std::vector<int16_t> s(frames);
        for (size_t i = 0; i < frames; ++i) s[i] = int16_t(std::lround(amplitude * std::sin(2 * kPi * frequency * double(i) / rate)));
        a.channels.push_back(std::move(s));
    }
    return a;
}

int zero_crossings(const std::vector<int16_t>& s) {
    int n = 0;
    for (size_t i = 1; i < s.size(); ++i) n += (s[i - 1] < 0) != (s[i] < 0);
    return n;
}

double rms(const std::vector<int16_t>& s) {
    double sum = 0;
    for (int16_t v : s) sum += double(v) * v;
    return std::sqrt(sum / double(s.size()));
}

bool has_note(const ConvertResult& r, const char* word) {
    for (const auto& n : r.notes) if (n.find(word) != std::string::npos) return true;
    return false;
}
}  // namespace

static void same_rate_same_channels_is_identity() {
    PcmAudio a = sine(32000, 0.1, 440);
    const auto original = a.channels;
    const auto r = convert_audio(std::move(a), {32000, 1, {}, {}});
    CHECK(r.audio && r.error.empty() && r.audio->rate == 32000 && r.audio->channels == original && !r.audio->loop && r.notes.empty());
}

static void rate_conversion_keeps_duration_and_frequency() {
    const PcmAudio a = sine(44100, 1.0, 440);
    const auto r = convert_audio(a, {32000, 1, {}, {}});
    CHECK(r.audio && r.audio->rate == 32000 && r.audio->frames() == 32000);
    const int original = zero_crossings(a.channels[0]), converted = zero_crossings(r.audio->channels[0]);
    CHECK(std::abs(original - converted) <= 1);
    CHECK(std::abs(rms(r.audio->channels[0]) / rms(a.channels[0]) - 1.0) < 0.02);
    const auto up = convert_audio(sine(22050, 1.0, 440), {48000, 1, {}, {}});   // upsampling keeps the pitch too
    CHECK(up.audio && up.audio->frames() == 48000 && std::abs(zero_crossings(up.audio->channels[0]) - 880) <= 2);
}

static void stereo_to_mono_averages() {
    PcmAudio a;
    a.rate = 32000;
    a.channels = {{1000, 2000, -3000}, {3000, 2000, -1001}};
    const auto r = convert_audio(a, {32000, 1, {}, {}});
    CHECK(r.audio && r.audio->channels.size() == 1 && (r.audio->channels[0] == std::vector<int16_t>{2000, 2000, -2001}) && has_note(r, "mono"));
}

static void mono_to_stereo_duplicates() {
    PcmAudio a;
    a.rate = 32000;
    a.channels = {{5, -6, 7}};
    const auto r = convert_audio(a, {32000, 2, {}, {}});
    CHECK(r.audio && r.audio->channels.size() == 2 && r.audio->channels[0] == a.channels[0] && r.audio->channels[1] == a.channels[0] && has_note(r, "stereo"));
}

static void loop_is_rescaled_and_audio_trimmed() {
    PcmAudio a = sine(44100, 3.0, 440);
    a.loop = {{44100, 88200}};   // 1 s to 2 s of a 3 s file
    const auto r = convert_audio(a, {32000, 1, {}, {}});
    CHECK(r.audio && r.audio->frames() == 64000 && r.audio->loop && r.audio->loop->first == 32000u && r.audio->loop->second == 64000u);
    PcmAudio b = sine(32000, 1.0, 440);
    const auto o = convert_audio(b, {32000, 1, {{8000, 24000}}, {}});   // the override wins and trims
    CHECK(o.audio && o.audio->frames() == 24000 && o.audio->loop && o.audio->loop->first == 8000u && o.audio->loop->second == 24000u);
    PcmAudio c = sine(32000, 1.0, 440);
    c.loop = {{100, 200}};
    const auto w = convert_audio(c, {32000, 1, {{500, 32000}}, {}});   // an override that already runs to the end trims nothing
    CHECK(w.audio && w.audio->frames() == 32000 && w.audio->loop && w.audio->loop->first == 500u && w.audio->loop->second == 32000u);
}

static void bad_loops_are_dropped_with_a_note() {
    for (const auto& bad : {std::pair<uint32_t, uint32_t>{500, 500}, {600, 500}, {10, 99999999}}) {
        PcmAudio a = sine(32000, 0.1, 440);
        a.loop = bad;
        const auto r = convert_audio(a, {32000, 1, {}, {}});
        CHECK(r.audio && !r.audio->loop && r.audio->frames() == a.frames() && has_note(r, "loop"));
    }
}

static void empty_and_overlong_are_errors() {
    PcmAudio none;
    none.rate = 32000;
    none.channels = {{}};
    CHECK(!convert_audio(none, {32000, 1, {}, {}}).audio && !convert_audio(none, {32000, 1, {}, {}}).error.empty());
    PcmAudio nothing;
    CHECK(!convert_audio(nothing, {32000, 1, {}, {}}).audio);
    PcmAudio long_audio;
    long_audio.rate = 8000;
    long_audio.channels = {std::vector<int16_t>(size_t(8000) * (kMaxSourceSeconds + 1))};
    const auto r = convert_audio(std::move(long_audio), {32000, 1, {}, {}});
    CHECK(!r.audio && r.error.find("30 minutes") != std::string::npos);
    CHECK(!convert_audio(sine(32000, 0.1, 440), {0, 1, {}, {}}).audio && !convert_audio(sine(32000, 0.1, 440), {32000, 3, {}, {}}).audio);
}

static void clipping_is_clamped_and_silence_survives() {
    PcmAudio loud = sine(44100, 0.2, 441, 1, 32767);
    const auto r = convert_audio(loud, {32000, 1, {}, {}});
    CHECK(r.audio);   // the filter overshoots at full scale: samples clamp instead of wrapping
    int peak = 0;
    for (int16_t v : r.audio->channels[0]) peak = std::max(peak, std::abs(int(v)));
    CHECK(peak >= 30000);
    PcmAudio silent;
    silent.rate = 44100;
    silent.channels = {std::vector<int16_t>(44100, 0)};
    const auto s = convert_audio(silent, {32000, 1, {}, {}});
    CHECK(s.audio && s.audio->frames() == 32000 && *std::max_element(s.audio->channels[0].begin(), s.audio->channels[0].end()) == 0);
}

static void cancel_stops_long_work() {
    PcmAudio a = sine(44100, 20.0, 440);
    int polls = 0;
    ConvertOptions options{32000, 1, {}, [&] { return ++polls > 2; }};
    const auto r = convert_audio(std::move(a), options);
    CHECK(!r.audio && r.error == "cancelled");
}

int main() {
    same_rate_same_channels_is_identity();
    rate_conversion_keeps_duration_and_frequency();
    stereo_to_mono_averages();
    mono_to_stereo_duplicates();
    loop_is_rescaled_and_audio_trimmed();
    bad_loops_are_dropped_with_a_note();
    empty_and_overlong_are_errors();
    clipping_is_clamped_and_silence_survives();
    cancel_stops_long_work();
    std::puts("test_audio_convert: ok");
    return 0;
}
