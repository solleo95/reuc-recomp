// Offline equivalence proof of a pacing fix record on dumped guest state (tools/step_audit/probe.py drives it).
// For each case p/q: the original function runs p times at the nominal 30 Hz step, and the record-adjusted function
// runs q times at step p/q from the same snapshot; every touched word of MEM1/MEM2 must agree (floats: relative 1e-5).
// Usage: step_probe <dump> <entry> <lr> <reg> <power> <this> <arg> <cases p/q,...> <k1 fields addr=nominal;...> <deny addr,...> <body|0> <frame>
//        (body/frame: the entry is already hooked; the probe runs its retained body as the unhooked original)
//        optional 13th argument "r4=@36,unhook=E:B:F": derived argument registers loaded from this+offset, and hooked entries
//        that run their retained body unhooked (validation of known sites only)
//        step_probe --selfcheck
// Prints one line: {"status":"pass|fail|unproven","reason":"...","max_rel":x}. A guest trap exits with status 3 (unproven).
#include "dispatch.h"
#include "guest_mem.h"
#include "game_pacing.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
extern const uc::FnEntry uc_functions[];
extern const size_t uc_function_count;
extern const uint32_t uc_text_begin, uc_text_end;
namespace {
uint32_t kStack = 0x817D0000, kStackLow = 0x817C0000, kStackHigh = 0x817D0100;   // moved into free MEM2 for dumps
constexpr size_t kMem1 = 24u << 20, kMem2 = 64u << 20;
float g_dt;
struct Unproven : std::runtime_error { using std::runtime_error::runtime_error; };
void clock_tail(ppc::Ctx& c) {
    ppc::wrf32(c, 0x804D331C, g_dt); ppc::wrf32(c, 0x804D3320, g_dt);
    c.lr = ppc::rd32(c, c.r[1] + 36); c.r[31] = ppc::rd32(c, c.r[1] + 28); c.r[1] = ppc::rd32(c, c.r[1]);
}
void denied(ppc::Ctx& c) { throw Unproven("calls a host-side function at LR " + std::to_string(c.lr)); }
uint32_t g_body, g_frame;   // original of a hooked entry: its retained body after a lowered stwu of g_frame bytes
void original_body(ppc::Ctx& c) { uc::pacing_call_body(c, g_frame, g_body); }
uint32_t g_unhook_body[4], g_unhook_frame[4];   // further hooked entries replaced by their unhooked originals
template <int N> void unhooked(ppc::Ctx& c) { uc::pacing_call_body(c, g_unhook_frame[N], g_unhook_body[N]); }
constexpr uc::Fn kUnhooked[4] = {unhooked<0>, unhooked<1>, unhooked<2>, unhooked<3>};
struct Job {
    uint32_t entry = 0, lr = 0, self = 0; unsigned reg = 1; int power = -1; double arg = 0;
    std::vector<std::pair<unsigned, unsigned>> cases;
    std::vector<std::pair<uint32_t, float>> k1;   // fields scaling with the step: address, nominal value
    std::vector<std::pair<unsigned, uint32_t>> derived;   // GPR n = word at this + offset (validation of known sites only)
};
void set_scale(ppc::Ctx& c, double s) {
    g_dt = float(s / 30.0);
    uc::game_pacing_init(true); ppc::wrf32(c, 0x804D3334, 60.f);
    c.r[1] = kStack; c.r[3] = 0x804D3318; c.lr = 0x80100000;
    ppc::wr32(c, 0x804D3340, 2); ppc::wr32(c, 0x804D3338, 1);
    hle_GameFrameWait(c);
}
void run(ppc::Ctx& c, const Job& job, unsigned times, double s, bool fixed) {
    set_scale(c, s);
    const double factor = fixed ? std::pow(s, job.power) : 1.0;
    for (unsigned i = 0; i < times; ++i) {
        for (auto [address, nominal] : job.k1) ppc::wrf32(c, address, float(nominal * s));
        c.r[1] = kStack; c.r[2] = 0x806CC700; c.r[13] = 0x806CA620; c.r[3] = job.self; c.lr = job.lr;
        for (auto [reg, offset] : job.derived) c.r[reg] = ppc::rd32(c, job.self + offset);
        c.f[job.reg] = c.ps1[job.reg] = double(float(float(job.arg * s) * factor));
        uc::call_indirect(c, job.entry);
    }
}
bool in_stack(uint32_t a) { return a >= kStackLow && a < kStackHigh; }
std::set<uint32_t> g_frame_words;   // words the frame boundary itself writes (clock, step coefficients): legitimately rate dependent
// Returns the number of mismatching words; max_rel gets the largest relative float error among matched words.
size_t compare(const uint8_t* a, const uint8_t* b, size_t size, uint32_t base, double& max_rel, uint32_t& first) {
    size_t bad = 0;
    for (size_t o = 0; o + 4 <= size; o += 4) {
        if (std::memcmp(a + o, b + o, 4) == 0 || in_stack(base + uint32_t(o)) || g_frame_words.count(base + uint32_t(o))) continue;
        uint32_t wa, wb; std::memcpy(&wa, a + o, 4); std::memcpy(&wb, b + o, 4);
        wa = _byteswap_ulong(wa); wb = _byteswap_ulong(wb);
        float fa, fb; std::memcpy(&fa, &wa, 4); std::memcpy(&fb, &wb, 4);
        const double denom = std::max(std::fabs(double(fa)), 1e-6);
        const double rel = std::fabs(double(fa) - double(fb)) / denom;
        if (std::isfinite(fa) && std::isfinite(fb) && rel <= 1e-5) { max_rel = std::max(max_rel, rel); continue; }
        if (!bad) first = base + uint32_t(o);
        ++bad;
    }
    return bad;
}
std::string prove(ppc::Ctx& c, uint8_t* mem, const Job& job) {
    std::vector<uint8_t> snapshot(kMem1 + kMem2), original(kMem1 + kMem2);
    std::memcpy(snapshot.data(), mem, kMem1); std::memcpy(snapshot.data() + kMem1, mem + 0x10000000, kMem2);
    auto restore = [&](const std::vector<uint8_t>& from) { std::memcpy(mem, from.data(), kMem1); std::memcpy(mem + 0x10000000, from.data() + kMem1, kMem2); };
    auto save = [&](std::vector<uint8_t>& to) { std::memcpy(to.data(), mem, kMem1); std::memcpy(to.data() + kMem1, mem + 0x10000000, kMem2); };
    double max_rel = 0;
    for (auto [p, q] : job.cases) {
        {   // Words the frame boundary alone changes between the two steps are excluded from the comparison.
            std::vector<uint8_t> a(kMem1 + kMem2), b(kMem1 + kMem2);
            restore(snapshot); set_scale(c, 1.0); save(a);
            restore(snapshot); set_scale(c, double(p) / q); save(b);
            g_frame_words.clear();
            for (size_t o = 0; o + 4 <= a.size(); o += 4)
                if (std::memcmp(a.data() + o, b.data() + o, 4))
                    g_frame_words.insert(o < kMem1 ? 0x80000000 + uint32_t(o) : 0x90000000 + uint32_t(o - kMem1));
            for (auto [address, nominal] : job.k1) g_frame_words.insert(address);   // inputs the probe sets per step
        }
        restore(snapshot); run(c, job, p, 1.0, false); save(original);
        double ignored = 0; uint32_t first = 0;
        if (!compare(snapshot.data(), original.data(), kMem1, 0x80000000, ignored, first) && !compare(snapshot.data() + kMem1, original.data() + kMem1, kMem2, 0x90000000, ignored, first))
            return "{\"status\":\"unproven\",\"reason\":\"the original call changed no state from this snapshot\"}";
        restore(snapshot); run(c, job, q, double(p) / q, true);
        std::vector<uint8_t> fixed(kMem1 + kMem2); save(fixed);
        size_t bad = compare(original.data(), fixed.data(), kMem1, 0x80000000, max_rel, first);
        if (!bad) bad = compare(original.data() + kMem1, fixed.data() + kMem1, kMem2, 0x90000000, max_rel, first);
        if (bad) {
            const size_t o = first >= 0x90000000 ? kMem1 + (first - 0x90000000) : first - 0x80000000;
            uint32_t wa, wb; std::memcpy(&wa, original.data() + o, 4); std::memcpy(&wb, fixed.data() + o, 4);
            wa = _byteswap_ulong(wa); wb = _byteswap_ulong(wb);
            float fa, fb; std::memcpy(&fa, &wa, 4); std::memcpy(&fb, &wb, 4);
            char line[320];
            std::snprintf(line, sizeof line, "{\"status\":\"fail\",\"reason\":\"case %u/%u: %zu words differ, first %08X original %08X (%g) fixed %08X (%g)\",\"max_rel\":%g}",
                          p, q, bad, first, wa, double(fa), wb, double(fb), max_rel);
            return line;
        }
    }
    char line[128];
    std::snprintf(line, sizeof line, "{\"status\":\"pass\",\"reason\":\"%zu cases exact\",\"max_rel\":%g}", job.cases.size(), max_rel);
    return line;
}
uint32_t hex(const char* s) { return uint32_t(std::strtoul(s, nullptr, 16)); }
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out; size_t at = 0;
    while (at <= s.size()) { auto end = s.find(sep, at); if (end == std::string::npos) end = s.size(); if (end > at) out.push_back(s.substr(at, end - at)); at = end + 1; }
    return out;
}
std::vector<uc::FnEntry> base_table(const std::set<uint32_t>& deny) {
    std::vector<uc::FnEntry> table(uc_functions, uc_functions + uc_function_count);
    for (auto& e : table) {
        if (e.addr == 0x80158208) e.fn = clock_tail;
        else if (deny.count(e.addr)) e.fn = denied;
    }
    return table;
}
// Synthetic bodies for --selfcheck (addresses inside the dispatch text range, below the translated code's end).
void body_scaled(ppc::Ctx& c) { ppc::wrf32(c, c.r[3] + 24, ppc::rdf32(c, c.r[3] + 24) + float(c.f[1]) * ppc::rdf32(c, c.r[3] + 16)); }
void body_damping(ppc::Ctx& c) { ppc::wrf32(c, c.r[3] + 24, ppc::rdf32(c, c.r[3] + 24) * 0.9f); }
void body_host(ppc::Ctx& c) { uc::call_indirect(c, 0x803FF300); }
int selfcheck(ppc::Ctx& c, uint8_t* mem) {
    std::set<uint32_t> deny{0x803FF300};
    auto table = base_table(deny);
    table.push_back({0x803FF000, body_scaled}); table.push_back({0x803FF100, body_damping});
    table.push_back({0x803FF200, body_host}); table.push_back({0x803FF300, denied});
    uc::dispatch_init(table.data(), table.size(), uc_text_begin, 0x80400000);
    Job job; job.lr = 0x80100000; job.self = 0x81000000; job.arg = 1.0 / 30; job.cases = {{1, 2}, {1, 4}, {1, 16}, {5, 24}, {2, 11}, {1, 8}};
    job.k1 = {{job.self + 16, 1.0f}};
    ppc::wrf32(c, job.self + 24, 10.f);
    const char* expect[] = {"pass", "fail", "unproven"};
    const uint32_t entries[] = {0x803FF000, 0x803FF100, 0x803FF200};
    int failures = 0;
    for (int i = 0; i < 3; ++i) {
        job.entry = entries[i];
        std::string got;
        try { got = prove(c, mem, job); } catch (const Unproven& e) { got = std::string("{\"status\":\"unproven\",\"reason\":\"") + e.what() + "\"}"; }
        const bool ok = got.find(std::string("\"status\":\"") + expect[i] + "\"") != std::string::npos;
        std::printf("%s %08X -> %s\n", ok ? "OK  " : "FAIL", entries[i], got.c_str());
        failures += !ok;
    }
    return failures ? 1 : 0;
}
}
#include <windows.h>
// A wild guest pointer from an incomplete context ends the probe; name the guest function so the report can say why.
static LONG CALLBACK report_access_violation(PEXCEPTION_POINTERS info) {
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        std::fprintf(stderr, "guest access violation in function %08X\n", uc::guest_function_for_host(info->ExceptionRecord->ExceptionAddress));
        std::fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
int main(int argc, char** argv) {
    AddVectoredExceptionHandler(1, report_access_violation);
    uc::guest_mem_init(); auto* mem = uc::guest_mem_base();
    ppc::Ctx c{}; c.mem = mem; c.r[2] = 0x806CC700; c.r[13] = 0x806CA620;
    if (argc == 2 && std::string(argv[1]) == "--selfcheck") return selfcheck(c, mem);
    if (argc != 13 && argc != 14) { std::fprintf(stderr, "usage: see header of tools/step_probe/step_probe.cpp\n"); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    in.read(reinterpret_cast<char*>(mem), kMem1); in.read(reinterpret_cast<char*>(mem) + 0x10000000, kMem2);
    if (!in) { std::printf("{\"status\":\"unproven\",\"reason\":\"cannot read dump\"}\n"); return 0; }
    {   // The probe's stack must not overlap live guest data: the highest all-zero 64 KiB block of MEM2.
        const uint8_t* m2 = mem + 0x10000000;
        bool found = false;
        for (size_t top = kMem2; top >= 0x20000 && !found; top -= 0x10000) {
            if (std::all_of(m2 + top - 0x10000, m2 + top, [](uint8_t b) { return b == 0; })) {
                kStackHigh = 0x90000000 + uint32_t(top) - 0x100; kStack = kStackHigh - 0x100; kStackLow = kStackHigh - 0x10000;
                found = true;
            }
        }
        if (!found) { std::printf("{\"status\":\"unproven\",\"reason\":\"no free memory for the probe stack\"}\n"); return 0; }
    }
    Job job; job.entry = hex(argv[2]); job.lr = hex(argv[3]); job.reg = unsigned(std::atoi(argv[4])); job.power = std::atoi(argv[5]);
    job.self = hex(argv[6]); job.arg = std::atof(argv[7]);
    for (auto& cs : split(argv[8], ',')) { auto pq = split(cs, '/'); job.cases.push_back({unsigned(std::stoul(pq[0])), unsigned(std::stoul(pq[1]))}); }
    for (auto& f : split(argv[9], ';')) { auto kv = split(f, '='); if (kv.size() == 2) job.k1.push_back({hex(kv[0].c_str()), std::stof(kv[1])}); }
    std::set<uint32_t> deny; for (auto& d : split(argv[10], ',')) deny.insert(hex(d.c_str()));
    std::vector<std::pair<uint32_t, int>> unhook;
    if (argc == 14) for (auto& d : split(argv[13], ',')) {   // "r4=@36": r4 = word at this+36; "unhook=E:B:F": entry E runs body B (frame F) unhooked
        auto kv = split(d, '=');
        if (kv.size() != 2) continue;
        if (kv[0] == "unhook") {
            auto ebf = split(kv[1], ':');
            if (ebf.size() == 3 && unhook.size() < 4) {
                g_unhook_body[unhook.size()] = hex(ebf[1].c_str()); g_unhook_frame[unhook.size()] = unsigned(std::stoul(ebf[2]));
                unhook.push_back({hex(ebf[0].c_str()), int(unhook.size())});
            }
        } else if (kv[0].size() > 1 && kv[1].size() > 1) job.derived.push_back({unsigned(std::stoul(kv[0].substr(1))), uint32_t(std::stoul(kv[1].substr(1)))});
    }
    auto table = base_table(deny);
    for (auto [entry, n] : unhook) for (auto& e : table) if (e.addr == entry) e.fn = kUnhooked[n];
    g_body = hex(argv[11]); g_frame = unsigned(std::atoi(argv[12]));
    if (g_body) for (auto& e : table) if (e.addr == job.entry) e.fn = original_body;   // hooked entry: probe the unhooked original
    uc::dispatch_init(table.data(), table.size(), uc_text_begin, 0x80400000);
    try { std::printf("%s\n", prove(c, mem, job).c_str()); }
    catch (const Unproven& e) { std::printf("{\"status\":\"unproven\",\"reason\":\"%s\"}\n", e.what()); }
    catch (const std::exception& e) { std::printf("{\"status\":\"unproven\",\"reason\":\"exception: %s\"}\n", e.what()); }
    return 0;
}
