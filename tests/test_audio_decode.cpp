#include "check.h"
#include "audio/decode.h"
#include "audio_fixtures.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace uc::mods;

namespace {
void le16(std::vector<uint8_t>& d, uint32_t v) { d.push_back(uint8_t(v)); d.push_back(uint8_t(v >> 8)); }
void le32(std::vector<uint8_t>& d, uint32_t v) { for (int i = 0; i < 4; ++i) d.push_back(uint8_t(v >> (8 * i))); }
void tag(std::vector<uint8_t>& d, const char* t) { d.insert(d.end(), t, t + 4); }

struct WavSpec { uint32_t format = 1, channels = 1, rate = 32000, bits = 16, block_align = 0; std::vector<uint8_t> data; std::vector<uint8_t> before_data, after_data; };

std::vector<uint8_t> make_wav(const WavSpec& s) {
    std::vector<uint8_t> d;
    tag(d, "RIFF"); le32(d, 0); tag(d, "WAVE");
    tag(d, "fmt "); le32(d, 16); le16(d, s.format); le16(d, s.channels); le32(d, s.rate); le32(d, s.rate * s.channels * s.bits / 8); le16(d, s.block_align ? s.block_align : s.channels * s.bits / 8); le16(d, s.bits);
    d.insert(d.end(), s.before_data.begin(), s.before_data.end());
    tag(d, "data"); le32(d, uint32_t(s.data.size())); d.insert(d.end(), s.data.begin(), s.data.end());
    if (s.data.size() % 2) d.push_back(0);
    d.insert(d.end(), s.after_data.begin(), s.after_data.end());
    const uint32_t riff = uint32_t(d.size() - 8);
    for (int i = 0; i < 4; ++i) d[4 + size_t(i)] = uint8_t(riff >> (8 * i));
    return d;
}

std::vector<uint8_t> chunk(const char* t, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> d;
    tag(d, t); le32(d, uint32_t(body.size())); d.insert(d.end(), body.begin(), body.end());
    if (body.size() % 2) d.push_back(0);
    return d;
}

std::vector<uint8_t> pcm16(std::initializer_list<int> values) {
    std::vector<uint8_t> d;
    for (int v : values) le16(d, uint32_t(uint16_t(int16_t(v))));
    return d;
}
}  // namespace

static void wav_pcm16_mono() {
    WavSpec s; s.data = pcm16({0, 1000, -1000, 32767, -32768});
    const auto r = read_wav(make_wav(s));
    CHECK(r.audio && r.error.empty() && r.audio->rate == 32000 && r.audio->channels.size() == 1);
    CHECK((r.audio->channels[0] == std::vector<int16_t>{0, 1000, -1000, 32767, -32768}) && !r.audio->loop);
}

static void wav_stereo_deinterleaves() {
    WavSpec s; s.channels = 2; s.rate = 44100; s.data = pcm16({1, 2, 3, 4, 5, 6});
    const auto r = read_wav(make_wav(s));
    CHECK(r.audio && r.audio->rate == 44100 && r.audio->channels.size() == 2);
    CHECK((r.audio->channels[0] == std::vector<int16_t>{1, 3, 5}) && (r.audio->channels[1] == std::vector<int16_t>{2, 4, 6}));
}

static void wav_8bit_24bit_32bit_and_float_scale_to_int16() {
    WavSpec e; e.bits = 8; e.data = {0, 128, 255};   // unsigned 8-bit: 0 is -1.0, 128 is 0, 255 about +1.0
    auto r = read_wav(make_wav(e));
    CHECK(r.audio && r.audio->channels[0][0] == -32768 && r.audio->channels[0][1] == 0 && r.audio->channels[0][2] >= 32256);
    WavSpec t; t.bits = 24; t.data = {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0};   // +0.5 and -0.5 (0x400000, 0xC00000)
    r = read_wav(make_wav(t));
    CHECK(r.audio && r.audio->channels[0][0] == 16384 && r.audio->channels[0][1] == -16384);
    WavSpec i32; i32.bits = 32; i32.data = {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0};   // 0x40000000, 0xC0000000
    r = read_wav(make_wav(i32));
    CHECK(r.audio && r.audio->channels[0][0] == 16384 && r.audio->channels[0][1] == -16384);
    WavSpec f; f.format = 3; f.bits = 32;
    const float values[] = {0.5f, -0.5f, 2.0f, -2.0f, 0.0f};
    for (float v : values) { uint32_t u; std::memcpy(&u, &v, 4); le32(f.data, u); }
    r = read_wav(make_wav(f));
    CHECK(r.audio && r.audio->channels[0].size() == 5);
    CHECK(std::abs(r.audio->channels[0][0] - 16384) <= 1 && std::abs(r.audio->channels[0][1] + 16384) <= 1);
    CHECK(r.audio->channels[0][2] == 32767 && r.audio->channels[0][3] == -32767 && r.audio->channels[0][4] == 0);   // clamped to [-1, 1]
}

static void wav_block_align_wins_over_the_bit_depth() {   // 24-bit samples in 4-byte containers (WAVE_FORMAT_PCM written by some tools)
    WavSpec s; s.bits = 24; s.block_align = 4; s.data = {0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0, 0x00};
    const auto r = read_wav(make_wav(s));
    CHECK(r.audio && r.audio->frames() == 2 && r.audio->channels[0][0] == 16384 && r.audio->channels[0][1] == -16384);
    WavSpec bad; bad.block_align = 1; bad.data = pcm16({1, 2});   // smaller than one sample: not usable
    CHECK(!read_wav(make_wav(bad)).audio);
}

static void wav_skips_unknown_chunks_and_pads_odd_sizes() {
    WavSpec s; s.data = pcm16({7, 8, 9});
    s.before_data = chunk("LIST", {1, 2, 3});    // odd size: followed by a pad byte
    s.after_data = chunk("junk", {9, 9});
    const auto r = read_wav(make_wav(s));
    CHECK(r.audio && (r.audio->channels[0] == std::vector<int16_t>{7, 8, 9}));
    WavSpec odd; odd.bits = 8; odd.data = {128, 128, 128};   // odd data size: the pad byte must not become a sample
    const auto o = read_wav(make_wav(odd));
    CHECK(o.audio && o.audio->channels[0].size() == 3);
}

static void wav_smpl_loop_is_read() {
    std::vector<uint8_t> body;
    for (int i = 0; i < 7; ++i) le32(body, 0);   // manufacturer, product, period, unity note, pitch fraction, SMPTE format and offset
    le32(body, 1);                               // loop count
    le32(body, 0);                               // sampler data
    le32(body, 0); le32(body, 0); le32(body, 1); le32(body, 3); le32(body, 0); le32(body, 0);   // id, type, start 1, end 3 (inclusive), fraction, play count
    WavSpec s; s.data = pcm16({1, 2, 3, 4, 5, 6}); s.before_data = chunk("smpl", body);
    const auto r = read_wav(make_wav(s));
    CHECK(r.audio && r.audio->loop && r.audio->loop->first == 1u && r.audio->loop->second == 4u);   // the end is exclusive
}

static void wav_loop_past_the_data_is_returned_as_is() {   // the converter, not the reader, drops a loop that does not fit and says so
    std::vector<uint8_t> body;
    for (int i = 0; i < 7; ++i) le32(body, 0);
    le32(body, 1); le32(body, 0);
    le32(body, 0); le32(body, 0); le32(body, 2); le32(body, 900); le32(body, 0); le32(body, 0);
    WavSpec s; s.data = pcm16({1, 2, 3, 4}); s.before_data = chunk("smpl", body);
    const auto r = read_wav(make_wav(s));
    CHECK(r.audio && r.audio->frames() == 4 && r.audio->loop && r.audio->loop->first == 2u && r.audio->loop->second == 901u);
}

static void wav_with_an_unknown_data_length_runs_to_the_end() {   // ffmpeg writing to a pipe leaves the size as 0xFFFFFFFF
    WavSpec s; s.data = pcm16({5, 6, 7});
    auto file = make_wav(s);
    const size_t at = 12 + 8 + 16;   // after RIFF header and fmt chunk: "data", size
    CHECK(std::memcmp(&file[at], "data", 4) == 0);
    for (int i = 0; i < 4; ++i) file[at + 4 + size_t(i)] = 0xFF;
    const auto r = read_wav(file);
    CHECK(r.audio && (r.audio->channels[0] == std::vector<int16_t>{5, 6, 7}));
}

static void wav_errors_name_the_problem() {
    CHECK(!read_wav({}).audio && !read_wav({}).error.empty());
    std::vector<uint8_t> not_riff(64, 0);
    CHECK(!read_wav(not_riff).audio);
    auto truncated = make_wav([] { WavSpec s; s.data = pcm16({1, 2, 3, 4}); return s; }());
    truncated.resize(30);
    CHECK(!read_wav(truncated).audio);
    auto longer = make_wav([] { WavSpec s; s.data = pcm16({1, 2, 3, 4}); return s; }());
    longer.resize(longer.size() - 4);   // the data chunk claims more than the file holds
    const auto r = read_wav(longer);
    CHECK(!r.audio && r.error.find("data") != std::string::npos);
    WavSpec none; none.channels = 0; none.data = pcm16({1});
    CHECK(!read_wav(make_wav(none)).audio);
    WavSpec three; three.channels = 3; three.data = pcm16({1, 2, 3});
    const auto t = read_wav(make_wav(three));
    CHECK(!t.audio && t.error.find("channel") != std::string::npos);
    WavSpec adpcm; adpcm.format = 2; adpcm.data = pcm16({1, 2});
    CHECK(!read_wav(make_wav(adpcm)).audio);
    WavSpec empty; empty.data = {};
    CHECK(!read_wav(make_wav(empty)).audio);   // zero frames
}

static void ogg_decodes_tone_length_and_rate() {
    const auto r = read_ogg(std::span<const uint8_t>(kToneOgg, sizeof kToneOgg));
    CHECK(r.audio && r.error.empty() && r.audio->rate == 32000 && r.audio->channels.size() == 1 && !r.audio->loop);
    CHECK(r.audio->frames() >= 3200 && r.audio->frames() <= 3600);
    int peak = 0;
    for (int16_t v : r.audio->channels[0]) peak = std::max(peak, std::abs(int(v)));
    CHECK(peak > 2000);   // a tone, not silence (ffmpeg's sine source peaks at 1/8 of full scale)
}

static void ogg_garbage_is_an_error() {
    std::vector<uint8_t> junk(500, 0x55);
    const auto r = read_ogg(junk);
    CHECK(!r.audio && !r.error.empty());
    CHECK(!read_ogg({}).audio);
    const auto cut = read_ogg(std::span<const uint8_t>(kToneOgg, 200));   // the headers are cut off
    CHECK(!cut.audio && !cut.error.empty());
}

static void ogg_decode_can_be_cancelled() {
    DecodeLimits limits;
    limits.cancel = [] { return true; };
    const auto r = read_ogg(std::span<const uint8_t>(kToneOgg, sizeof kToneOgg), limits);
    CHECK(!r.audio && r.error == "cancelled");
    DecodeLimits roomy;
    roomy.max_seconds = 60;
    CHECK(read_ogg(std::span<const uint8_t>(kToneOgg, sizeof kToneOgg), roomy).audio);   // a short file is within any limit
}

static void ogg_loop_comments() {
    const auto r = read_ogg(std::span<const uint8_t>(kLoopOgg, sizeof kLoopOgg));
    CHECK(r.audio && r.audio->loop && r.audio->loop->first == 100u && r.audio->loop->second == 600u);
}

int main() {
    wav_pcm16_mono();
    wav_stereo_deinterleaves();
    wav_8bit_24bit_32bit_and_float_scale_to_int16();
    wav_block_align_wins_over_the_bit_depth();
    wav_skips_unknown_chunks_and_pads_odd_sizes();
    wav_smpl_loop_is_read();
    wav_loop_past_the_data_is_returned_as_is();
    wav_with_an_unknown_data_length_runs_to_the_end();
    wav_errors_name_the_problem();
    ogg_decodes_tone_length_and_rate();
    ogg_garbage_is_an_error();
    ogg_decode_can_be_cancelled();
    ogg_loop_comments();
    std::puts("test_audio_decode: ok");
    return 0;
}
