#include "audio/brstm.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace uc::mods {
namespace {
constexpr uint32_t kPcmBlockBytes = 0x2000, kPcmBlockSamples = kPcmBlockBytes / 2;   // per channel
constexpr uint32_t kHeaderSize = 0x40, kDataHeader = 0x20, kAdpcmParamBytes = 0x30, kMarker = 0x01000000;

uint32_t align(uint32_t n, uint32_t a) { return (n + a - 1) / a * a; }

struct Writer {
    std::vector<uint8_t> bytes;
    void u8(size_t at, uint8_t v) { bytes[at] = v; }
    void u16(size_t at, uint32_t v) { bytes[at] = uint8_t(v >> 8); bytes[at + 1] = uint8_t(v); }
    void u32(size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) bytes[at + size_t(i)] = uint8_t(v >> (24 - 8 * i)); }
};

struct Reader {
    std::span<const uint8_t> d;
    bool has(size_t at, size_t n) const { return at <= d.size() && n <= d.size() - at; }
    uint32_t u32(size_t at) const { return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3]; }
    uint32_t u16(size_t at) const { return uint32_t(d[at]) << 8 | d[at + 1]; }
};

struct Parsed {
    BrstmHeader header;
    uint32_t data_start = 0, blocks = 0, block_size = 0, block_samples = 0, last_size = 0, last_samples = 0, last_padded = 0;
    size_t stream_base = 0;   // HEAD + 8: the offsets of the tables are relative to it
    size_t channel_table = 0;
};

// The stream info of the HEAD chunk; false when the first bytes are not a BRSTM header.
bool parse_stream(const Reader& r, Parsed& p) {
    if (!r.has(0, 0x28) || std::memcmp(r.d.data(), "RSTM", 4) != 0) return false;
    const uint32_t head = r.u32(0x10);
    if (!r.has(head, 8 + 0x18) || std::memcmp(&r.d[head], "HEAD", 4) != 0) return false;
    const size_t base = size_t(head) + 8;
    if (!r.has(base, 24)) return false;
    const uint32_t stream = r.u32(base + 4), channels = r.u32(base + 20);
    const size_t si = base + stream;
    if (!r.has(si, 0x2C)) return false;
    p.stream_base = base;
    p.channel_table = base + channels;
    p.header.codec = r.d[si];
    p.header.looped = r.d[si + 1] != 0;
    p.header.channels = r.d[si + 2];
    p.header.rate = r.u16(si + 4);
    p.header.loop_start = r.u32(si + 8);
    p.header.frames = r.u32(si + 12);
    p.data_start = r.u32(si + 16);
    p.blocks = r.u32(si + 20);
    p.block_size = r.u32(si + 24);
    p.block_samples = r.u32(si + 28);
    p.last_size = r.u32(si + 32);
    p.last_samples = r.u32(si + 36);
    p.last_padded = r.u32(si + 40);
    return true;
}

int16_t clamp16(int v) { return int16_t(std::clamp(v, -32768, 32767)); }
}  // namespace

std::vector<uint8_t> write_brstm_pcm16(const PcmAudio& audio) {
    const size_t channels = audio.channels.size();
    if (channels < 1 || channels > 2) throw std::invalid_argument("a BRSTM needs 1 or 2 channels");
    if (audio.rate == 0 || audio.rate > 65535) throw std::invalid_argument("the sample rate must be 1 to 65535");
    const size_t frames = audio.frames();
    if (frames == 0) throw std::invalid_argument("no samples");
    if (frames > 0x7FFFFFFFu / 2 / channels) throw std::invalid_argument("too many samples");
    for (const auto& c : audio.channels) if (c.size() != frames) throw std::invalid_argument("the channels differ in length");
    if (audio.loop && (audio.loop->second != frames || audio.loop->first >= frames)) throw std::invalid_argument("a loop must run from its start to the last sample");

    const uint32_t blocks = uint32_t((frames + kPcmBlockSamples - 1) / kPcmBlockSamples);
    const uint32_t last_samples = uint32_t(frames - size_t(blocks - 1) * kPcmBlockSamples), last_size = last_samples * 2, last_padded = align(last_size, 0x20);
    // HEAD: three table entries, stream info, one track (a list of channel numbers), the channel table, and per channel an info record and 0x30 bytes of ADPCM
    // parameters (zero: unused by PCM16). The same layout as the game's own mono (0xC0 bytes) and stereo (0x100) streams.
    const uint32_t channel_info_at = 0x68 + 8 * uint32_t(channels), channel_info_stride = 8 + kAdpcmParamBytes;
    const uint32_t head_size = align(channel_info_at + channel_info_stride * uint32_t(channels), 0x20);
    const uint32_t adpc_size = align(8 + blocks * uint32_t(channels) * 4, 0x20);
    const uint32_t head_pos = kHeaderSize, adpc_pos = head_pos + head_size, data_pos = adpc_pos + adpc_size;
    const uint32_t audio_bytes = (blocks - 1) * kPcmBlockBytes * uint32_t(channels) + last_padded * uint32_t(channels);
    const uint32_t data_size = align(kDataHeader + audio_bytes, 0x20);

    Writer w;
    w.bytes.assign(size_t(data_pos) + data_size, 0);
    std::memcpy(&w.bytes[0], "RSTM", 4);
    w.u16(4, 0xFEFF); w.u16(6, 0x0100); w.u32(8, uint32_t(w.bytes.size())); w.u16(0x0C, kHeaderSize); w.u16(0x0E, 2);
    w.u32(0x10, head_pos); w.u32(0x14, head_size); w.u32(0x18, adpc_pos); w.u32(0x1C, adpc_size); w.u32(0x20, data_pos); w.u32(0x24, data_size);

    uint8_t* const head = &w.bytes[head_pos];
    auto put32 = [&](uint32_t at, uint32_t v) { for (int i = 0; i < 4; ++i) head[at + uint32_t(i)] = uint8_t(v >> (24 - 8 * i)); };
    auto put16 = [&](uint32_t at, uint32_t v) { head[at] = uint8_t(v >> 8); head[at + 1] = uint8_t(v); };
    std::memcpy(head, "HEAD", 4); put32(4, head_size);
    put32(0x08, kMarker); put32(0x0C, 0x18);   // stream info
    put32(0x10, kMarker); put32(0x14, 0x4C);   // track table
    put32(0x18, kMarker); put32(0x1C, 0x5C);   // channel table
    const uint32_t si = 0x20;
    head[si] = 1; head[si + 1] = audio.loop ? 1 : 0; head[si + 2] = uint8_t(channels);
    put16(si + 4, audio.rate);
    put32(si + 8, audio.loop ? audio.loop->first : 0); put32(si + 0xC, uint32_t(frames)); put32(si + 0x10, data_pos + kDataHeader);
    put32(si + 0x14, blocks); put32(si + 0x18, kPcmBlockBytes); put32(si + 0x1C, kPcmBlockSamples);
    put32(si + 0x20, last_size); put32(si + 0x24, last_samples); put32(si + 0x28, last_padded);
    put32(si + 0x2C, 0x400); put32(si + 0x30, 4);   // ADPCM seek table spacing: unused for PCM16, kept like the originals
    head[0x54] = 1;                                  // track table: one track
    put32(0x58, kMarker); put32(0x5C, 0x58);
    head[0x60] = uint8_t(channels); head[0x62] = channels == 2 ? 1 : 0;   // the track's channel list: 0, 1
    head[0x64] = uint8_t(channels);                  // channel table
    for (uint32_t c = 0; c < channels; ++c) {
        const uint32_t info = channel_info_at + c * channel_info_stride;   // inside HEAD
        put32(0x68 + 8 * c, kMarker); put32(0x6C + 8 * c, info - 8);       // table entry -> channel info (relative to HEAD + 8)
        put32(info, kMarker); put32(info + 4, info);                       // channel info -> ADPCM parameters at info + 8 (relative to HEAD + 8: info)
    }

    std::memcpy(&w.bytes[adpc_pos], "ADPC", 4); w.u32(adpc_pos + 4, adpc_size);
    std::memcpy(&w.bytes[data_pos], "DATA", 4); w.u32(data_pos + 4, data_size);
    size_t at = size_t(data_pos) + kDataHeader;
    for (uint32_t block = 0; block < blocks; ++block) {
        const bool last = block + 1 == blocks;
        const size_t count = last ? last_samples : kPcmBlockSamples;
        for (size_t c = 0; c < channels; ++c) {
            for (size_t i = 0; i < count; ++i) {
                const uint16_t v = uint16_t(audio.channels[c][size_t(block) * kPcmBlockSamples + i]);
                w.bytes[at + 2 * i] = uint8_t(v >> 8); w.bytes[at + 2 * i + 1] = uint8_t(v);
            }
            at += last ? last_padded : kPcmBlockBytes;
        }
    }
    return w.bytes;
}

std::optional<BrstmHeader> read_brstm_header(std::span<const uint8_t> first_bytes) {
    Reader r{first_bytes};
    Parsed p;
    if (!parse_stream(r, p)) return std::nullopt;
    return p.header;
}

std::optional<PcmAudio> read_brstm(std::span<const uint8_t> file) {
    Reader r{file};
    Parsed p;
    if (!parse_stream(r, p)) return std::nullopt;
    const size_t channels = p.header.channels;
    if (channels < 1 || channels > 2 || p.blocks == 0 || p.header.frames == 0 || p.block_size == 0) return std::nullopt;
    if (p.header.codec != 1 && p.header.codec != 2) return std::nullopt;

    int16_t coefs[2][16] = {};
    if (p.header.codec == 2) {
        if (!r.has(p.channel_table, 4) || r.d[p.channel_table] < channels) return std::nullopt;
        for (size_t c = 0; c < channels; ++c) {
            const size_t entry = p.channel_table + 4 + c * 8;
            if (!r.has(entry, 8)) return std::nullopt;
            const size_t info = p.stream_base + r.u32(entry + 4);
            if (!r.has(info, 8)) return std::nullopt;
            const size_t params = p.stream_base + r.u32(info + 4);
            if (!r.has(params, 32)) return std::nullopt;
            for (size_t i = 0; i < 16; ++i) coefs[c][i] = int16_t(r.u16(params + 2 * i));
        }
    }

    PcmAudio audio;
    audio.rate = p.header.rate;
    audio.channels.assign(channels, {});
    int y1[2] = {}, y2[2] = {};
    for (uint32_t block = 0; block < p.blocks; ++block) {
        const bool last = block + 1 == p.blocks;
        const uint32_t stride = last ? p.last_padded : p.block_size;
        const uint32_t samples = last ? p.last_samples : p.block_samples;
        for (size_t c = 0; c < channels; ++c) {
            const size_t start = size_t(p.data_start) + size_t(block) * p.block_size * channels + c * stride;
            if (!r.has(start, stride)) return std::nullopt;
            auto& out = audio.channels[c];
            if (p.header.codec == 1) {
                if (uint64_t(samples) * 2 > stride) return std::nullopt;
                for (uint32_t i = 0; i < samples; ++i) out.push_back(int16_t(r.u16(start + 2 * i)));
                continue;
            }
            uint32_t produced = 0;
            for (size_t frame = 0; frame + 8 <= stride && produced < samples; frame += 8) {
                const uint8_t ps = r.d[start + frame];
                const int scale = 1 << (ps & 15), predictor = ps >> 4;
                if (predictor > 7) return std::nullopt;
                const int c1 = coefs[c][predictor * 2], c2 = coefs[c][predictor * 2 + 1];
                for (int i = 0; i < 14 && produced < samples; ++i, ++produced) {
                    const uint8_t byte = r.d[start + frame + 1 + size_t(i / 2)];
                    int nibble = i % 2 == 0 ? byte >> 4 : byte & 15;
                    if (nibble >= 8) nibble -= 16;
                    const int v = clamp16((nibble * scale * 2048 + 1024 + c1 * y1[c] + c2 * y2[c]) >> 11);
                    out.push_back(int16_t(v));
                    y2[c] = y1[c]; y1[c] = v;
                }
            }
            if (produced != samples) return std::nullopt;
        }
    }
    if (audio.frames() != p.header.frames) return std::nullopt;
    if (p.header.looped && p.header.loop_start < p.header.frames) audio.loop = {{p.header.loop_start, p.header.frames}};
    return audio;
}

}  // namespace uc::mods
