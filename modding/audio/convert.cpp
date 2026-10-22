#include "audio/convert.h"
#include <algorithm>
#include <cmath>

namespace uc::mods {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kZeroCrossings = 32;   // taps per side at the lower of the two rates
constexpr int kTableResolution = 512;
constexpr double kKaiserBeta = 9.0;  // about -90 dB stop band

double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

// sinc(u) windowed by a Kaiser window that ends at u = kZeroCrossings; indexed by u * kTableResolution.
const std::vector<float>& kernel_table() {
    static const std::vector<float> table = [] {
        std::vector<float> t(size_t(kZeroCrossings) * kTableResolution + 2, 0.0f);
        const double norm = bessel_i0(kKaiserBeta);
        for (size_t i = 0; i + 1 < t.size(); ++i) {
            const double u = double(i) / kTableResolution;
            if (u >= kZeroCrossings) break;
            const double sinc = u == 0 ? 1.0 : std::sin(kPi * u) / (kPi * u);
            const double ratio = u / kZeroCrossings;
            t[i] = float(sinc * bessel_i0(kKaiserBeta * std::sqrt(1.0 - ratio * ratio)) / norm);
        }
        return t;
    }();
    return table;
}

int16_t to_int16(float v) { return int16_t(std::clamp(std::lround(v), -32768L, 32767L)); }

// One channel from `from` Hz to `to` Hz. Returns false when cancelled.
bool resample(const std::vector<int16_t>& in, uint32_t from, uint32_t to, size_t out_frames, std::vector<int16_t>& out, const std::function<bool()>& cancel) {
    const auto& table = kernel_table();
    const double step = double(from) / double(to);              // source samples per output sample
    const double cutoff = std::min(1.0, double(to) / double(from));
    const double support = kZeroCrossings / cutoff;             // source samples on each side
    const double table_scale = cutoff * kTableResolution;
    out.resize(out_frames);
    for (size_t i = 0; i < out_frames; ++i) {
        if ((i & 0xFFFF) == 0 && cancel && cancel()) return false;
        const double t = double(i) * step;
        const long first = std::max(0L, long(std::ceil(t - support))), last = std::min(long(in.size()) - 1, long(std::floor(t + support)));
        float sum = 0;
        for (long j = first; j <= last; ++j) {
            const double position = std::abs(double(j) - t) * table_scale;
            const size_t index = size_t(position);
            const float fraction = float(position - double(index));
            sum += float(in[size_t(j)]) * (table[index] + (table[index + 1] - table[index]) * fraction);
        }
        out[i] = to_int16(sum * float(cutoff));
    }
    return true;
}
}  // namespace

ConvertResult convert_audio(PcmAudio source, const ConvertOptions& options) {
    ConvertResult result;
    const auto fail = [&](std::string message) { result.audio.reset(); result.error = std::move(message); return std::move(result); };
    if (options.target_rate == 0 || options.target_rate > 65535) return fail("the target sample rate is not valid");
    if (options.target_channels < 1 || options.target_channels > 2) return fail("the target needs 1 or 2 channels");
    if (source.channels.empty() || source.channels.size() > 2 || source.rate == 0) return fail("the audio has no usable channels or sample rate");
    const size_t frames = source.frames();
    if (frames == 0) return fail("the audio is empty");
    for (const auto& c : source.channels) if (c.size() != frames) return fail("the channels differ in length");
    if (frames > uint64_t(kMaxSourceSeconds) * source.rate) return fail("the audio is longer than 30 minutes");

    std::optional<std::pair<uint32_t, uint32_t>> loop = options.loop_override ? options.loop_override : source.loop;
    if (loop && (loop->first >= loop->second || loop->second > frames)) {
        result.notes.push_back("the loop points (" + std::to_string(loop->first) + " to " + std::to_string(loop->second) + ") do not fit the audio and are ignored");
        loop.reset();
    }

    // Channels first: a downmix halves the resampling work.
    auto& channels = source.channels;
    if (channels.size() == 2 && options.target_channels == 1) {
        std::vector<int16_t> mono(frames);
        for (size_t i = 0; i < frames; ++i) mono[i] = int16_t((int(channels[0][i]) + int(channels[1][i])) >> 1);
        channels = {std::move(mono)};
        result.notes.push_back("the audio is stereo and the game's stream is mono: mixed down to mono");
    }
    const bool duplicate = channels.size() == 1 && options.target_channels == 2;   // done after resampling: one channel is filtered, not two
    if (duplicate) result.notes.push_back("the audio is mono and the game's stream is stereo: played on both channels (stereo)");

    PcmAudio out;
    out.rate = options.target_rate;
    size_t out_frames = frames;
    if (source.rate == options.target_rate) {
        out.channels = std::move(channels);
    } else {
        out_frames = size_t(std::llround(double(frames) * options.target_rate / source.rate));
        if (out_frames == 0) return fail("the audio is too short to convert");
        out.channels.resize(channels.size());
        for (size_t c = 0; c < channels.size(); ++c)
            if (!resample(channels[c], source.rate, options.target_rate, out_frames, out.channels[c], options.cancel)) return fail("cancelled");
        result.notes.push_back("resampled from " + std::to_string(source.rate) + " Hz to " + std::to_string(options.target_rate) + " Hz");
    }

    if (duplicate) out.channels.push_back(out.channels[0]);

    if (loop) {
        const double ratio = double(out_frames) / double(frames);
        const auto start = uint32_t(std::llround(double(loop->first) * ratio));
        const auto end = uint32_t(std::min<long long>(std::llround(double(loop->second) * ratio), static_cast<long long>(out_frames)));
        if (start < end) {
            for (auto& c : out.channels) c.resize(end);   // a BRSTM loop runs to the end of the stream: what follows the loop end is cut
            out.loop = {{start, end}};
        } else {
            result.notes.push_back("the loop is shorter than one output sample and is ignored");
        }
    }
    result.audio = std::move(out);
    return result;
}

}  // namespace uc::mods
