#include "counter_probe.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
namespace uc {
namespace {
constexpr size_t kMem1 = 24u << 20, kMem2 = 64u << 20, kWords = (kMem1 + kMem2) / 4;
struct Probe {
    std::string path, go;
    uint32_t steps = 0, taken = 0;
    bool enabled = false, armed = false, written = false;
    double game_seconds = 0;
    std::vector<uint32_t> previous, last_int_delta, hits_int, hits_float, changed;
    std::vector<float> last_float_delta;
    Probe() {
        const char* spec = std::getenv("UC_COUNTER_PROBE");
        if (!spec) return;
        std::string text(spec);
        const auto bar = text.find('|');
        steps = bar == std::string::npos ? 240 : uint32_t(std::strtoul(text.c_str() + bar + 1, nullptr, 10));
        path = text.substr(0, bar);
        go = path + ".go";
        enabled = steps >= 3;
    }
};
Probe& probe() { static Probe instance; return instance; }
uint32_t word_at(const ppc::Ctx& c, size_t index) {
    const size_t byte = index < kMem1 / 4 ? index * 4 : 0x10000000 + (index - kMem1 / 4) * 4;
    uint32_t value; std::memcpy(&value, c.mem + byte, 4);
    return __builtin_bswap32(value);
}
void write_report(Probe& p) {
    std::FILE* out = std::fopen(p.path.c_str(), "w");
    if (!out) return;
    std::fprintf(out, "# steps %u game_seconds %.4f mean_step %.6f\n", p.taken, p.game_seconds, p.game_seconds / std::max(1u, p.taken - 1));
    const uint32_t needed = (p.taken - 2) * 7 / 10;
    for (size_t i = 0; i < kWords; ++i) {
        const bool as_int = p.hits_int[i] >= needed && p.last_int_delta[i] != 0;
        const bool as_float = p.hits_float[i] >= needed && p.last_float_delta[i] != 0 && std::isfinite(p.last_float_delta[i]);
        if (!as_int && !as_float) continue;
        const uint32_t address = i < kMem1 / 4 ? 0x80000000u + uint32_t(i) * 4 : 0x90000000u + uint32_t(i - kMem1 / 4) * 4;
        std::fprintf(out, "%08X int %d %u float %.9g %u changed %u\n", address, as_int ? int32_t(p.last_int_delta[i]) : 0, as_int ? p.hits_int[i] : 0u,
                     as_float ? p.last_float_delta[i] : 0.f, as_float ? p.hits_float[i] : 0u, p.changed[i]);
    }
    std::fclose(out);
}
}
void counter_probe_step(const ppc::Ctx& c, float timestep) {
    auto& p = probe();
    if (!p.enabled || p.written || !c.mem) return;
    if (!p.armed) {
        if (!std::filesystem::exists(p.go)) return;
        p.armed = true;
        p.previous.assign(kWords, 0); p.last_int_delta.assign(kWords, 0); p.hits_int.assign(kWords, 0); p.hits_float.assign(kWords, 0);
        p.changed.assign(kWords, 0); p.last_float_delta.assign(kWords, 0.f);
    }
    const bool first = p.taken == 0;
    for (size_t i = 0; i < kWords; ++i) {
        const uint32_t value = word_at(c, i), before = p.previous[i];
        p.previous[i] = value;
        if (first || value == before) continue;
        ++p.changed[i];
        const int32_t delta = int32_t(value - before);
        if (p.taken >= 2 && delta == int32_t(p.last_int_delta[i])) ++p.hits_int[i];
        p.last_int_delta[i] = uint32_t(delta);
        const float now = std::bit_cast<float>(value), was = std::bit_cast<float>(before), step = now - was;
        if (std::isfinite(step) && p.taken >= 2 && std::fabs(step - p.last_float_delta[i]) <= 1e-4f * std::fabs(step) + 1e-9f) ++p.hits_float[i];
        p.last_float_delta[i] = std::isfinite(step) ? step : 0.f;
    }
    if (!first) p.game_seconds += timestep;
    if (++p.taken >= p.steps) { write_report(p); p.written = true; p.previous = {}; p.last_int_delta = {}; p.hits_int = {}; p.hits_float = {}; p.changed = {}; p.last_float_delta = {}; }
}
}
