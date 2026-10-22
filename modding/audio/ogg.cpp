#include "audio/decode.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.c"   // declarations only: the decoder itself is built from the same file as C (modding/CMakeLists.txt)
}

namespace uc::mods {
namespace {
constexpr uint64_t kMaxFrames = 0x7FFFFFFF / 4;   // keeps every size calculation far from overflow; the converter's 30-minute limit is much lower

const char* open_error(int code) {
    switch (code) {
        case VORBIS_need_more_data: return "the file is cut short";
        case VORBIS_invalid_api_mixing: case VORBIS_feature_not_supported: return "this kind of Vorbis stream is not supported";
        case VORBIS_outofmem: return "out of memory";
        case VORBIS_missing_capture_pattern: case VORBIS_invalid_stream_structure_version: case VORBIS_continued_packet_flag_invalid: return "not an Ogg Vorbis file";
        case VORBIS_invalid_setup: case VORBIS_invalid_first_page: case VORBIS_bad_packet_type: return "damaged Vorbis headers";
        default: return "cannot decode the Vorbis stream";
    }
}

bool starts_with_nocase(const char* text, const char* key) {
    for (; *key; ++text, ++key) if (std::toupper(static_cast<unsigned char>(*text)) != *key) return false;
    return *text == '=';
}

std::optional<uint32_t> number_after_equals(const char* comment) {
    const char* value = std::strchr(comment, '=');
    if (!value || !*++value) return std::nullopt;
    char* end = nullptr;
    const unsigned long long n = std::strtoull(value, &end, 10);
    if (end == value || *end != '\0' || n > 0xFFFFFFFFull) return std::nullopt;
    return uint32_t(n);
}
}  // namespace

DecodeResult read_ogg(std::span<const uint8_t> file, const DecodeLimits& limits) {
    if (file.size() < 4 || file.size() > 0x7FFFFFFFu) return {std::nullopt, "not an Ogg Vorbis file"};
    int error = 0;
    stb_vorbis* v = stb_vorbis_open_memory(file.data(), int(file.size()), &error, nullptr);
    if (!v) return {std::nullopt, open_error(error)};
    struct Close { stb_vorbis* v; ~Close() { stb_vorbis_close(v); } } close{v};
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    if (info.channels < 1 || info.channels > 2) return {std::nullopt, std::to_string(info.channels) + " channels are not supported (1 or 2)"};
    if (info.sample_rate == 0 || info.sample_rate > 384000) return {std::nullopt, "the sample rate " + std::to_string(info.sample_rate) + " is not usable"};

    PcmAudio audio;
    audio.rate = info.sample_rate;
    audio.channels.assign(size_t(info.channels), {});
    std::vector<short> buffer(4096 * size_t(info.channels));
    const uint64_t max_frames = limits.max_seconds ? std::min<uint64_t>(kMaxFrames, uint64_t(limits.max_seconds) * info.sample_rate) : kMaxFrames;
    for (;;) {
        if (limits.cancel && limits.cancel()) return {std::nullopt, "cancelled"};
        const int frames = stb_vorbis_get_samples_short_interleaved(v, info.channels, buffer.data(), int(buffer.size()));
        if (frames <= 0) break;
        if (audio.channels[0].size() + uint64_t(frames) > max_frames) return {std::nullopt, limits.max_seconds ? "the audio is longer than " + std::to_string(limits.max_seconds / 60) + " minutes" : "the audio is far too long"};
        for (int i = 0; i < frames; ++i)
            for (int c = 0; c < info.channels; ++c) audio.channels[size_t(c)].push_back(buffer[size_t(i) * size_t(info.channels) + size_t(c)]);
    }
    if (audio.frames() == 0) return {std::nullopt, "the Vorbis stream holds no samples"};

    std::optional<uint32_t> start, length, end;
    const stb_vorbis_comment comments = stb_vorbis_get_comment(v);
    for (int i = 0; i < comments.comment_list_length; ++i) {
        const char* c = comments.comment_list[i];
        if (starts_with_nocase(c, "LOOPSTART")) start = number_after_equals(c);
        else if (starts_with_nocase(c, "LOOPLENGTH")) length = number_after_equals(c);
        else if (starts_with_nocase(c, "LOOPEND")) end = number_after_equals(c);
    }
    if (start && (length || end)) {
        const uint64_t stop = length ? uint64_t(*start) + *length : *end;
        if (stop <= 0xFFFFFFFFull) audio.loop = {{*start, uint32_t(stop)}};   // not checked here: convert_audio drops a loop that does not fit and says so
    }
    return {std::move(audio), {}};
}

}  // namespace uc::mods
