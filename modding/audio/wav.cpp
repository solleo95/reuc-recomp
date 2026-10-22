#include "audio/decode.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace uc::mods {
namespace {
uint32_t le16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

DecodeResult fail(std::string message) { return {std::nullopt, std::move(message)}; }
}  // namespace

DecodeResult read_wav(std::span<const uint8_t> file) {
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) != 0 || std::memcmp(&file[8], "WAVE", 4) != 0) return fail("not a WAV file (no RIFF/WAVE header)");
    uint32_t format = 0, channels = 0, rate = 0, bits = 0, block_align = 0;
    bool have_format = false;
    std::span<const uint8_t> data;
    bool have_data = false;
    std::optional<std::pair<uint32_t, uint32_t>> loop;
    for (size_t at = 12; at + 8 <= file.size();) {
        const uint8_t* header = &file[at];
        const size_t size = le32(header + 4), body = at + 8;
        const bool is_data = std::memcmp(header, "data", 4) == 0, is_fmt = std::memcmp(header, "fmt ", 4) == 0;
        if (is_data && size == 0xFFFFFFFFu) {   // a stream written without knowing its length (ffmpeg to a pipe): the data runs to the end of the file
            data = file.subspan(body);
            have_data = true;
            break;
        }
        if (size > file.size() - body) {
            if (is_data) return fail("the data chunk is longer than the file (cut short)");
            if (is_fmt) return fail("the fmt chunk is cut short");
            break;   // a damaged trailing chunk: what was read so far still counts
        }
        if (is_fmt) {
            if (size < 16) return fail("the fmt chunk is too small");
            format = le16(&file[body]); channels = le16(&file[body + 2]); rate = le32(&file[body + 4]); block_align = le16(&file[body + 12]); bits = le16(&file[body + 14]);
            if (format == 0xFFFE && size >= 26) format = le16(&file[body + 24]);   // WAVE_FORMAT_EXTENSIBLE: the sub-format starts with the real tag
            have_format = true;
        } else if (is_data) {
            data = file.subspan(body, size);
            have_data = true;
        } else if (std::memcmp(header, "smpl", 4) == 0 && size >= 60 && le32(&file[body + 28]) >= 1) {
            const uint32_t start = le32(&file[body + 44]), end = le32(&file[body + 48]);
            if (end < 0xFFFFFFFFu) loop = {{start, end + 1}};   // the chunk's end is the last sample of the loop; convert_audio drops one that does not fit and says so
        }
        at = body + size + (size & 1);
    }
    if (!have_format) return fail("the file has no fmt chunk");
    if (!have_data) return fail("the file has no data chunk");
    if (channels < 1 || channels > 2) return fail(std::to_string(channels) + " channels are not supported (1 or 2)");
    if (rate == 0 || rate > 384000) return fail("the sample rate " + std::to_string(rate) + " is not usable");
    const bool is_float = format == 3;
    if (!(format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !(is_float && bits == 32))
        return fail("unsupported sample format (tag " + std::to_string(format) + ", " + std::to_string(bits) + " bits): use 8/16/24/32-bit PCM or 32-bit float");
    const size_t bytes = bits / 8;
    if (block_align == 0) block_align = uint32_t(bytes * channels);   // a writer that left it out
    if (block_align < bytes * channels) return fail("the block alignment " + std::to_string(block_align) + " is smaller than one sample frame");   // wider containers (24 bits in 4 bytes) are fine
    const size_t frames = data.size() / block_align;
    if (frames == 0) return fail("the data chunk holds no samples");

    PcmAudio audio;
    audio.rate = rate;
    audio.channels.assign(channels, std::vector<int16_t>(frames));
    for (size_t i = 0; i < frames; ++i) {
        for (uint32_t c = 0; c < channels; ++c) {
            const uint8_t* p = &data[i * block_align + c * bytes];
            int16_t v = 0;
            if (is_float) {
                float f;
                std::memcpy(&f, p, 4);
                v = std::isfinite(f) ? int16_t(std::lround(std::clamp(f, -1.0f, 1.0f) * 32767.0f)) : int16_t(0);
            } else if (bits == 8) {
                v = int16_t((int(p[0]) - 128) * 256);
            } else if (bits == 16) {
                v = int16_t(le16(p));
            } else if (bits == 24) {
                v = int16_t(int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 16);   // the three bytes are the top of a 32-bit word
            } else {
                v = int16_t(int32_t(le32(p)) >> 16);
            }
            audio.channels[c][i] = v;
        }
    }
    audio.loop = loop;
    return {std::move(audio), {}};
}

}  // namespace uc::mods
