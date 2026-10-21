#include "state_dump.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
namespace uc {
namespace {
constexpr size_t kMem1 = 24u << 20, kMem2 = 64u << 20;
struct Dump {
    std::string prefix, go;
    std::vector<double> times;
    size_t next = 0;
    double elapsed = 0;
    bool enabled = false, armed = false;
    Dump() {
        const char* spec = std::getenv("UC_STATE_DUMP");
        if (!spec) return;
        std::string text(spec);
        const auto bar = text.find('|');
        if (bar == std::string::npos) return;
        prefix = text.substr(0, bar); go = prefix + ".go";
        for (size_t at = bar + 1; at < text.size();) {
            auto end = text.find(',', at); if (end == std::string::npos) end = text.size();
            times.push_back(std::strtod(text.substr(at, end - at).c_str(), nullptr));
            at = end + 1;
        }
        enabled = !times.empty();
    }
};
Dump& dump() { static Dump instance; return instance; }
struct FnCount {
    std::string prefix;
    unsigned snapshots = 0;
    double elapsed = 0, total = 0; uint64_t steps = 0, total_steps = 0;
    std::FILE* per_step = nullptr;
    FnCount() {
        const char* spec = std::getenv("UC_FN_COUNT");
        if (!spec) return;
        prefix = spec;
        g_fn_counts = static_cast<uint32_t*>(std::calloc(size_t(1) << 23, sizeof(uint32_t)));
        g_fn_touched = static_cast<uint32_t*>(std::calloc(size_t(1) << 20, sizeof(uint32_t)));
        g_fn_lr = static_cast<uint32_t*>(std::calloc(size_t(1) << 23, sizeof(uint32_t)));
        g_fn_arg = static_cast<uint32_t*>(std::calloc(size_t(1) << 23, sizeof(uint32_t)));
    }
    void reset() { for (uint32_t i = 0; i < g_fn_touched_n; ++i) g_fn_counts[g_fn_touched[i]] = 0; g_fn_touched_n = 0; }
};
}
uint32_t* g_fn_counts = nullptr;
uint32_t* g_fn_touched = nullptr;
uint32_t* g_fn_lr = nullptr;
uint32_t* g_fn_arg = nullptr;
static uint64_t vm_step = 0;
void vm_trace(uint32_t address, const ppc::Ctx& c) {
    static std::FILE* file = [] { const char* path = std::getenv("UC_VM_TRACE"); return path ? std::fopen(path, "w") : nullptr; }();
    if (!file || !g_fn_counts) return;
    std::fprintf(file, "V %llu %08X r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X f1 %.6g f2 %.6g f3 %.6g f4 %.6g\n", (unsigned long long)vm_step, address, c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], c.f[1], c.f[2], c.f[3], c.f[4]);
}
uint32_t g_fn_touched_n = 0;
void state_dump_step(const ppc::Ctx& c, float timestep) {
    static FnCount counts;
    if (g_fn_counts) {
        ++vm_step; counts.elapsed += timestep; ++counts.steps; counts.total += timestep; ++counts.total_steps;
        if (!counts.per_step && std::filesystem::exists(counts.prefix + ".auto")) counts.per_step = std::fopen((counts.prefix + "_steps.txt").c_str(), "w");
        if (counts.per_step) {   // one line per step: step number, game time, then "<address>:<calls>" of every function the step entered
            std::fprintf(counts.per_step, "S %llu %.4f", (unsigned long long)counts.total_steps, counts.total);
            for (uint32_t i = 0; i < g_fn_touched_n; ++i) std::fprintf(counts.per_step, " %X:%u@%X/%X", unsigned(0x80000000u + (g_fn_touched[i] << 2)), g_fn_counts[g_fn_touched[i]], g_fn_lr[g_fn_touched[i]], g_fn_arg[g_fn_touched[i]]);
            std::fputc(10, counts.per_step); std::fflush(counts.per_step);
            counts.reset();
        } else if (std::filesystem::exists(counts.prefix + ".snap")) {
            std::FILE* out = std::fopen((counts.prefix + "_" + std::to_string(counts.snapshots++) + ".txt").c_str(), "w");
            if (out) {
                std::fprintf(out, "# game_seconds %.4f steps %llu\n", counts.elapsed, (unsigned long long)counts.steps);
                for (uint32_t i = 0; i < g_fn_touched_n; ++i) std::fprintf(out, "%08X %u\n", unsigned(0x80000000u + (g_fn_touched[i] << 2)), g_fn_counts[g_fn_touched[i]]);
                std::fclose(out);
            }
            counts.reset();
            counts.elapsed = 0; counts.steps = 0;
            std::error_code ignored; std::filesystem::remove(counts.prefix + ".snap", ignored);
        }
    }
    auto& d = dump();
    if (!d.enabled || d.next >= d.times.size() || !c.mem) return;
    if (!d.armed) { if (!std::filesystem::exists(d.go)) return; d.armed = true; }
    d.elapsed += timestep;
    if (d.elapsed < d.times[d.next]) return;
    char name[32]; std::snprintf(name, sizeof name, "%.2f", d.times[d.next]);
    std::FILE* out = std::fopen((d.prefix + "_" + name + ".bin").c_str(), "wb");
    if (out) {
        std::fwrite(c.mem, 1, kMem1, out);
        std::fwrite(c.mem + 0x10000000, 1, kMem2, out);
        std::fclose(out);
    }
    ++d.next;
}
}
