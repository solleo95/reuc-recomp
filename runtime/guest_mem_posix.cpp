#include "guest_mem.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace uc {
static uint8_t* g_base;

// Same layout as guest_mem_win.cpp: guest addresses index a 4 GiB range directly (host address = base + guest address), the three RAM blocks
// are memory files mapped at every address the console reaches them by, everything else (the hardware registers at 0xCC000000 included)
// stays unmapped and faults.
namespace {
struct View { const char* what; uint64_t size; uint64_t at[3]; unsigned count; };
constexpr uint64_t kSpan = 0x100000000ull;
const View kViews[] = {
    {"MEM1", 0x01800000, {0x00000000, 0x80000000, 0xC0000000}, 3},
    {"MEM2", 0x04000000, {0x10000000, 0x90000000, 0xD0000000}, 3},
    {"locked cache", 0x00010000, {0x20000000, 0xE0000000, 0}, 2},   // 64 KB: the view granularity; the console has 16 KB
};

void unmap_all(uint8_t* base) { munmap(base, kSpan); }

bool map_all(uint8_t* base, const int (&files)[3]) {
    for (unsigned v = 0; v < 3; ++v)
        for (unsigned i = 0; i < kViews[v].count; ++i)
            if (mmap(base + kViews[v].at[i], kViews[v].size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, files[v], 0) == MAP_FAILED) return false;
    return true;
}

[[noreturn]] void give_up(const char* what, int error) {
    std::fprintf(stderr, "fatal: cannot map guest memory (%s: %s)\n", what, std::strerror(error));
    std::exit(4);
}
}  // namespace

void guest_mem_init() {
    if (g_base) return;
    int files[3];
    for (unsigned v = 0; v < 3; ++v) {
        files[v] = memfd_create(kViews[v].what, MFD_CLOEXEC);
        if (files[v] < 0) give_up(kViews[v].what, errno);
        if (ftruncate(files[v], off_t(kViews[v].size))) give_up(kViews[v].what, errno);
    }
    // Reserve the whole range first (no access, no commit), then map the views over it; another thread may take part of it in between, so try again.
    int error = 0;
    for (int attempt = 0; attempt < 16 && !g_base; ++attempt) {
        void* reserved = mmap(nullptr, kSpan, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (reserved == MAP_FAILED) { error = errno; break; }
        if (map_all(static_cast<uint8_t*>(reserved), files)) g_base = static_cast<uint8_t*>(reserved);
        else { error = errno; unmap_all(static_cast<uint8_t*>(reserved)); }
    }
    for (int file : files) close(file);   // the mappings keep the memory alive
    if (!g_base) give_up("4 GiB range", error);
}
uint8_t* guest_mem_base() { return g_base; }
}  // namespace uc
