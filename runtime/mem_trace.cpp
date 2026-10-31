#include "mem_trace.h"
#ifdef UC_TRACE_MEM
#include "ppc_rt.h"
#include "vi.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace uc::mem_trace {
namespace {
constexpr uint32_t kFrameWindow = 0x20000;   // below a call's entry stack pointer: the frames of that call and of everything it calls (private to the call)
struct Open {
    uint64_t id, parent;
    uint32_t function, sp;
    int64_t start;
    std::vector<uint64_t> reads, writes;   // (lo << 32) | hi, not merged yet
    std::vector<std::string> hles;
};
thread_local std::vector<Open> stack;
std::once_flag configured;
std::vector<uint32_t> functions;
uint32_t window_from = 0, window_frames = 120;
const char* flag_path = nullptr;
bool flag_seen = false;
uint32_t last_flag_check = ~0u;
std::mutex out_mutex;
std::FILE* out = nullptr;
uint64_t next_id = 1;

void configure() {
    if (const char* list = std::getenv("UC_TRACE_MEM_FUNCS")) {
        for (const char* p = list; *p;) { char* end; const unsigned long v = std::strtoul(p, &end, 16); if (end == p) { ++p; continue; } functions.push_back(uint32_t(v)); p = end; }
    }
    std::sort(functions.begin(), functions.end());
    if (const char* from = std::getenv("UC_TRACE_MEM_FROM")) window_from = uint32_t(std::strtoul(from, nullptr, 10));
    if (const char* frames = std::getenv("UC_TRACE_MEM_FRAMES")) window_frames = uint32_t(std::strtoul(frames, nullptr, 10));
    flag_path = std::getenv("UC_TRACE_MEM_FLAG");
    const char* path = std::getenv("UC_TRACE_MEM_OUT");
    out = std::fopen(path ? path : "mem-trace.txt", "w");
    std::atexit(flush);
}
void coalesce(std::vector<uint64_t>& v) {
    std::sort(v.begin(), v.end());
    std::vector<uint64_t> merged;
    for (uint64_t r : v) {
        const uint32_t lo = uint32_t(r >> 32), hi = uint32_t(r);
        if (!merged.empty() && lo <= uint32_t(merged.back())) { if (hi > uint32_t(merged.back())) merged.back() = (merged.back() & 0xFFFFFFFF00000000ull) | hi; }
        else merged.push_back(r);
    }
    v.swap(merged);
}
void add(std::vector<uint64_t>& v, uint32_t ea, uint32_t size) {
    v.push_back(uint64_t(ea) << 32 | uint64_t(ea + size));
    if (v.size() > (1u << 21)) coalesce(v);
}
bool private_frame(const Open& o, uint32_t ea) { return ea < o.sp && ea >= o.sp - kFrameWindow; }
void write_ranges(std::string& line, const char* tag, const std::vector<uint64_t>& v) {
    line += tag;
    char text[32];
    for (uint64_t r : v) { std::snprintf(text, sizeof text, " %X:%X", uint32_t(r >> 32), uint32_t(r)); line += text; }
    line += '\n';
}
bool in_ram(uint32_t v) { return (v >= 0x80000000u && v < 0x81800000u) || (v >= 0x90000000u && v < 0x94000000u); }
}  // namespace

Scope enter(uint32_t function, const ppc::Ctx& c) {
    std::call_once(configured, configure);
    if (!std::binary_search(functions.begin(), functions.end(), function)) return {};
    if (stack.empty()) {   // a call starts only inside the window; calls started inside it are followed to their end
        const uint32_t retrace = uint32_t(vi_stats().retraces);
        if (flag_path && !flag_seen) {   // UC_TRACE_MEM_FLAG: the window opens at the first retrace after that file exists (a test scenario creates it at the scene of interest)
            if (retrace == last_flag_check) return {};
            last_flag_check = retrace;
            if (std::FILE* flag = std::fopen(flag_path, "rb")) { std::fclose(flag); flag_seen = true; window_from = retrace; }
            else return {};
        }
        if (retrace < window_from || retrace >= window_from + window_frames) return {};
    }
    std::lock_guard<std::mutex> lock(out_mutex);
    const uint64_t id = next_id++;
    stack.push_back({id, stack.empty() ? 0 : stack.back().id, function, c.r[1], std::chrono::steady_clock::now().time_since_epoch().count(), {}, {}, {}});
    return Scope(true);
}
void read(uint32_t ea, uint32_t size) {
    if (stack.empty() || private_frame(stack.back(), ea)) return;
    add(stack.back().reads, ea, size);
}
void write(uint32_t ea, uint32_t size) {
    if (stack.empty() || private_frame(stack.back(), ea)) return;
    add(stack.back().writes, ea, size);
}
void hle(const char* name, const ppc::Ctx& c) {
    if (stack.empty()) return;
    Open& o = stack.back();
    o.hles.emplace_back(name);
    const std::string_view n(name);
    for (const char* quiet : {"OSDisableInterrupts", "OSRestoreInterrupts", "Leaf_", "SaveGpr", "RestGpr", "SaveFpr", "RestFpr"}) if (n.find(quiet) != std::string_view::npos) return;
    for (int r = 3; r <= 6; ++r) if (in_ram(c.r[r])) { add(o.reads, c.r[r], 64); add(o.writes, c.r[r], 64); }   // unknown effects: every pointer argument may be read and written
}
Scope::~Scope() {
    if (!on) return;
    Open o = std::move(stack.back());
    stack.pop_back();
    coalesce(o.reads); coalesce(o.writes);
    std::sort(o.hles.begin(), o.hles.end()); o.hles.erase(std::unique(o.hles.begin(), o.hles.end()), o.hles.end());
    const int64_t ticks = std::chrono::steady_clock::now().time_since_epoch().count() - o.start;
    if (!stack.empty()) {   // the caller's sets include this call's (minus what falls into the caller's own frame)
        Open& parent = stack.back();
        for (uint64_t r : o.reads) if (!(private_frame(parent, uint32_t(r >> 32)) && uint32_t(r) <= parent.sp)) parent.reads.push_back(r);
        for (uint64_t r : o.writes) if (!(private_frame(parent, uint32_t(r >> 32)) && uint32_t(r) <= parent.sp)) parent.writes.push_back(r);
        parent.hles.insert(parent.hles.end(), o.hles.begin(), o.hles.end());
    }
    std::string line = "C " + std::to_string(o.id) + " " + std::to_string(o.parent) + " " + [&] { char t[16]; std::snprintf(t, sizeof t, "%08X", o.function); return std::string(t); }() + " " + std::to_string(ticks) + "\n";
    write_ranges(line, "R", o.reads); write_ranges(line, "W", o.writes);
    line += "H";
    for (const auto& h : o.hles) line += "\t" + h;   // tab separated: a name has spaces
    line += '\n';
    std::lock_guard<std::mutex> lock(out_mutex);
    if (out) std::fputs(line.c_str(), out);
}
void flush() { if (out) std::fflush(out); }
}  // namespace uc::mem_trace
#endif
