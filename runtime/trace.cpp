#include "trace.h"
#include "ppc_rt.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace {
std::map<uint32_t, std::string> g_syms;
struct Ring { uint32_t a[256]; uint32_t pos = 0; int tid = 0; mutable std::mutex mutex; };
std::mutex g_rings_m;
std::vector<Ring*> g_rings;
Ring& ring() {
    thread_local Ring* r = [] { auto* x = new Ring{}; std::lock_guard<std::mutex> lk(g_rings_m); x->tid = int(g_rings.size()); g_rings.push_back(x); return x; }();
    return *r;
}
void dump_ring(std::FILE* out, const Ring& r, int count) {
    std::unique_lock lock(r.mutex, std::try_to_lock);
    if (!lock.owns_lock()) { std::fprintf(out, "  (trace busy)\n"); return; }
    uint32_t n = std::min<uint32_t>(uint32_t(count), std::min<uint32_t>(r.pos, 256));
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t a = r.a[(r.pos - n + i) & 255];
        std::fprintf(out, "  %08X %s\n", a, uc::symbol_name(a).c_str());
    }
    if (r.pos == 0) std::fprintf(out, "  (no trace: build with UC_TRACE=ON)\n");
}
}  // namespace

namespace ppc { void trace_enter(uint32_t a) { Ring& r = ring(); std::lock_guard lock(r.mutex); r.a[r.pos++ & 255] = a; } }

namespace uc {
void symbols_load(const std::string& path) {
    std::ifstream f(path);
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string a, n;
        if (ls >> a >> n) g_syms[uint32_t(std::stoul(a, nullptr, 16))] = n;
    }
}
std::string symbol_name(uint32_t a) {
    auto it = g_syms.upper_bound(a);
    if (it == g_syms.begin()) return {};
    --it;
    if (a - it->first > 0x4000) return {};
    if (it->first == a) return it->second;
    char b[24];
    std::snprintf(b, sizeof b, "+0x%X", a - it->first);
    return it->second + b;
}
void trace_dump(std::FILE* out, int count) { dump_ring(out, ring(), count); }
void trace_dump_all(std::FILE* out, int count) {
    std::lock_guard<std::mutex> lk(g_rings_m);
    for (const Ring* r : g_rings) { std::fprintf(out, "host thread %d:\n", r->tid); dump_ring(out, *r, count); }
}
}  // namespace uc
