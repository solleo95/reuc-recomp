#include "guest_mem.h"
#include <cstdio>
#include <cstdlib>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace uc {
static uint8_t* g_base;

// Guest addresses index the 4 GiB range directly (host address = base + guest address), so translated loads and stores need no address mask. The
// three RAM blocks are page-file sections mapped at every address the console reaches them by:
//   MEM1 24 MB: physical 0x00000000, cached 0x80000000, uncached 0xC0000000      MEM2 64 MB: 0x10000000, 0x90000000, 0xD0000000
//   locked cache 16 KB (dcbz_l, psq and lfs in the SDK use it): 0x20000000 (physical alias), 0xE0000000
// Everything else (the hardware registers at 0xCC000000 included) stays unmapped; the translated code tests for the registers before it touches memory.
namespace {
struct View { const char* what; uint64_t size; uint64_t at[3]; unsigned count; };
constexpr uint64_t kSpan = 0x100000000ull;
const View kViews[] = {
    {"MEM1", 0x01800000, {0x00000000, 0x80000000, 0xC0000000}, 3},
    {"MEM2", 0x04000000, {0x10000000, 0x90000000, 0xD0000000}, 3},
    {"locked cache", 0x00010000, {0x20000000, 0xE0000000, 0}, 2},   // 64 KB: the view granularity; the console has 16 KB
};

bool map_all(uint8_t* base, HANDLE (&sections)[3]) {
    for (unsigned v = 0; v < 3; ++v)
        for (unsigned i = 0; i < kViews[v].count; ++i)
            if (!MapViewOfFileEx(sections[v], FILE_MAP_ALL_ACCESS, 0, 0, size_t(kViews[v].size), base + kViews[v].at[i])) {
                for (unsigned u = 0; u <= v; ++u)
                    for (unsigned j = 0; j < kViews[u].count; ++j)
                        if (u < v || j < i) UnmapViewOfFile(base + kViews[u].at[j]);
                return false;
            }
    return true;
}
}  // namespace

void guest_mem_init() {
    if (g_base) return;
    HANDLE sections[3];
    for (unsigned v = 0; v < 3; ++v) {
        sections[v] = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(kViews[v].size >> 32), DWORD(kViews[v].size), nullptr);
        if (!sections[v]) { std::fprintf(stderr, "fatal: cannot create the %s section (error %lu)\n", kViews[v].what, GetLastError()); std::exit(4); }
    }
    // Find a free 4 GiB range, release it and map the views into it; another thread may take part of it in between, so try again.
    for (int attempt = 0; attempt < 16 && !g_base; ++attempt) {
        auto* probe = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSpan, MEM_RESERVE, PAGE_NOACCESS));
        if (!probe) break;
        VirtualFree(probe, 0, MEM_RELEASE);
        if (map_all(probe, sections)) g_base = probe;
    }
    if (!g_base) { std::fprintf(stderr, "fatal: cannot map guest memory\n"); std::exit(4); }
}
uint8_t* guest_mem_base() { return g_base; }
}  // namespace uc
