#include "check.h"
#include "audio/brstm.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

using namespace uc::mods;

namespace {
uint32_t be32(const std::vector<uint8_t>& d, size_t o) { return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3]; }

PcmAudio ramp(size_t frames, int channels, uint32_t rate = 32000) {
    PcmAudio a;
    a.rate = rate;
    for (int c = 0; c < channels; ++c) {
        std::vector<int16_t> samples(frames);
        for (size_t i = 0; i < frames; ++i) samples[i] = int16_t((int(i) * 7 + c * 1000) % 30000 - 15000);
        a.channels.push_back(std::move(samples));
    }
    return a;
}

bool throws_invalid(const PcmAudio& a) {
    try { write_brstm_pcm16(a); } catch (const std::invalid_argument&) { return true; }
    return false;
}
}  // namespace

static void pcm16_round_trip_mono() {
    const PcmAudio a = ramp(1000, 1);
    const auto file = write_brstm_pcm16(a);
    const auto back = read_brstm(file);
    CHECK(back && back->rate == 32000 && back->channels == a.channels && !back->loop);
    const auto header = read_brstm_header(file);
    CHECK(header && header->codec == 1 && header->channels == 1 && header->rate == 32000 && header->frames == 1000 && !header->looped);
}

static void pcm16_round_trip_stereo_with_loop() {
    PcmAudio a = ramp(4096 * 3 + 777, 2, 48000);
    a.loop = {{5000, uint32_t(a.frames())}};
    const auto file = write_brstm_pcm16(a);
    const auto back = read_brstm(file);
    CHECK(back && back->rate == 48000 && back->channels == a.channels);
    CHECK(back->loop && back->loop->first == 5000u && back->loop->second == a.frames());
    const auto header = read_brstm_header(file);
    CHECK(header && header->looped && header->loop_start == 5000u && header->frames == a.frames() && header->channels == 2);
}

static void header_fields() {
    const auto file = write_brstm_pcm16(ramp(10000, 2));
    CHECK(std::memcmp(file.data(), "RSTM", 4) == 0 && file[4] == 0xFE && file[5] == 0xFF);
    CHECK(be32(file, 8) == file.size());   // file size field
    const uint32_t head = be32(file, 0x10), adpc = be32(file, 0x18), adpc_size = be32(file, 0x1C), data = be32(file, 0x20), data_size = be32(file, 0x24);
    CHECK(std::memcmp(&file[head], "HEAD", 4) == 0 && std::memcmp(&file[adpc], "ADPC", 4) == 0 && std::memcmp(&file[data], "DATA", 4) == 0);
    CHECK(adpc + adpc_size == data && data + data_size == file.size());
    for (uint32_t i = adpc + 8; i < adpc + adpc_size; ++i) CHECK(file[i] == 0);   // the seek table is zero-filled
    const uint32_t stream = head + 8 + be32(file, head + 12);
    CHECK(file[stream] == 1 && file[stream + 2] == 2);                 // codec PCM16, two channels
    CHECK(be32(file, stream + 0x10) == data + 0x20);                    // audio data starts after the DATA chunk header
    CHECK(be32(file, stream + 0x14) == 3 && be32(file, stream + 0x18) == 0x2000 && be32(file, stream + 0x1C) == 4096);   // 10000 frames: 3 blocks of 4096 samples
    CHECK(be32(file, stream + 0x24) == 10000 - 2 * 4096 && be32(file, stream + 0x20) == (10000 - 2 * 4096) * 2);   // last block samples and bytes
    CHECK(be32(file, stream + 0x28) % 0x20 == 0);                       // padded to 32 bytes
}

static void rejects_bad_input() {
    PcmAudio none;
    none.rate = 32000;
    CHECK(throws_invalid(none));                                  // no channels
    CHECK(throws_invalid(ramp(100, 3)));                          // three channels
    CHECK(throws_invalid(ramp(0, 1)));                            // no samples
    PcmAudio ragged = ramp(100, 2);
    ragged.channels[1].pop_back();
    CHECK(throws_invalid(ragged));
    PcmAudio no_rate = ramp(100, 1, 0);
    CHECK(throws_invalid(no_rate));
    PcmAudio bad_loop = ramp(100, 1);
    bad_loop.loop = {{10, 50}};                                   // does not end at the last sample
    CHECK(throws_invalid(bad_loop));
    CHECK(!read_brstm(std::vector<uint8_t>(10, 0)));              // not a BRSTM
    auto file = write_brstm_pcm16(ramp(5000, 1));
    file.resize(file.size() - 100);
    CHECK(!read_brstm(file));                                     // cut short
    auto wrapped = write_brstm_pcm16(ramp(100, 1));
    const uint32_t stream = be32(wrapped, 0x10) + 8 + be32(wrapped, be32(wrapped, 0x10) + 12);
    wrapped[stream + 0x24] = 0x80;                                // last block "samples" of 0x80000000: samples * 2 wraps in 32 bits
    wrapped[stream + 0x25] = wrapped[stream + 0x26] = wrapped[stream + 0x27] = 0;
    CHECK(!read_brstm(wrapped));
}

static void adpcm_decode_matches_reference() {
    // One mono channel, three DSP-ADPCM frames (42 samples), built by hand from the layout of the game's own streams (sine.brstm is the 352-byte example).
    std::vector<uint8_t> f(0x40 + 0xC0 + 0x20 + 0x40, 0);
    const auto put32 = [&](size_t o, uint32_t v) { f[o] = uint8_t(v >> 24); f[o + 1] = uint8_t(v >> 16); f[o + 2] = uint8_t(v >> 8); f[o + 3] = uint8_t(v); };
    std::memcpy(&f[0], "RSTM", 4); f[4] = 0xFE; f[5] = 0xFF; f[6] = 1; f[7] = 0;
    put32(8, uint32_t(f.size())); f[0x0D] = 0x40; f[0x0F] = 3;
    put32(0x10, 0x40); put32(0x14, 0xC0); put32(0x18, 0x100); put32(0x1C, 0x20); put32(0x20, 0x120); put32(0x24, 0x40);
    std::memcpy(&f[0x40], "HEAD", 4); put32(0x44, 0xC0);
    const size_t base = 0x48;
    put32(base + 0, 0x01000000); put32(base + 4, 0x18); put32(base + 8, 0x01000000); put32(base + 12, 0x4C); put32(base + 16, 0x01000000); put32(base + 20, 0x5C);
    const size_t stream = base + 0x18;
    f[stream] = 2; f[stream + 2] = 1; f[stream + 4] = 0x7D; f[stream + 5] = 0x00;   // codec 2, one channel, 32000 Hz
    put32(stream + 0xC, 42); put32(stream + 0x10, 0x140); put32(stream + 0x14, 1); put32(stream + 0x18, 0x2000); put32(stream + 0x1C, 0x3800);
    put32(stream + 0x20, 24); put32(stream + 0x24, 42); put32(stream + 0x28, 0x20);
    const size_t channels = base + 0x5C;   // channel table: count, then (marker, offset) per channel
    f[channels] = 1; put32(channels + 4, 0x01000000); put32(channels + 8, 0x68);
    put32(base + 0x68, 0x01000000); put32(base + 0x6C, 0x70);
    const size_t coefs = base + 0x70;   // predictor 0 = (1.0, 0) in 11-bit fixed point: c1 = 2048, c2 = 0; predictor 1 = (0, 0)
    f[coefs] = 0x08; f[coefs + 1] = 0x00;
    std::memcpy(&f[0x100], "ADPC", 4); put32(0x104, 0x20);
    std::memcpy(&f[0x120], "DATA", 4); put32(0x124, 0x40);
    // Frame 0: scale 2^0, predictor 0, nibbles 1 2 3 4 5 6 7 -8 -7 -6 -5 -4 -3 -2; frames 1 and 2: header 0 (scale 1, predictor 0) and zero nibbles.
    f[0x140] = 0x00;
    const int nibbles[14] = {1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2};
    for (int i = 0; i < 14; i += 2) f[0x141 + i / 2] = uint8_t(((nibbles[i] & 15) << 4) | (nibbles[i + 1] & 15));
    const auto audio = read_brstm(f);
    CHECK(audio && audio->rate == 32000 && audio->channels.size() == 1 && audio->frames() == 42);
    // with c1 = 2048 (1.0) and scale 1 each sample is y = nibble + previous y: a running sum
    int expected = 0;
    for (int i = 0; i < 14; ++i) { expected += nibbles[i]; CHECK(audio->channels[0][size_t(i)] == expected); }
    for (int i = 14; i < 42; ++i) CHECK(audio->channels[0][size_t(i)] == expected);   // zero nibbles keep the value
}

int main() {
    pcm16_round_trip_mono();
    pcm16_round_trip_stereo_with_loop();
    header_fields();
    rejects_bad_input();
    adpcm_decode_matches_reference();
    std::puts("test_brstm: ok");
    return 0;
}
