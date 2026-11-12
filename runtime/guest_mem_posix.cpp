#include "guest_mem.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <fcntl.h>
#endif

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
        for (unsigned i = 0; i < kViews[v].count; ++i) {
            if (mmap(base + kViews[v].at[i], kViews[v].size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, files[v], 0) == MAP_FAILED) return false;
            // 2 MB pages for the RAM files (shmem THP "advise" mode, before the first touch): the translated code reaches all of MEM1/MEM2, far more than
            // the TLB covers with 4 KB pages. Only a hint: without THP the kernel keeps 4 KB pages.
#ifdef MADV_HUGEPAGE
            madvise(base + kViews[v].at[i], kViews[v].size, MADV_HUGEPAGE);
#endif
        }
    return true;
}

[[noreturn]] void give_up(const char* what, int error) {
    std::fprintf(stderr, "fatal: cannot map guest memory (%s: %s)\n", what, std::strerror(error));
    std::exit(4);
}

// A memory file for one view. macOS has no memfd: a POSIX shared memory object (names are at most 31 bytes), unlinked at once so that only the
// mappings keep it; 16 KB pages divide every view size and address in kViews.
int memory_file(unsigned view) {
#if defined(__APPLE__)
    char name[32];
    std::snprintf(name, sizeof name, "/uc.%d.%u", int(getpid()), view);
    const int file = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (file >= 0) shm_unlink(name);
    return file;
#else
    return memfd_create(kViews[view].what, MFD_CLOEXEC);
#endif
}
}  // namespace

void guest_mem_init() {
    if (g_base) return;
    int files[3];
    for (unsigned v = 0; v < 3; ++v) {
        files[v] = memory_file(v);
        if (files[v] < 0) give_up(kViews[v].what, errno);
        if (ftruncate(files[v], off_t(kViews[v].size))) give_up(kViews[v].what, errno);
    }
    // Reserve the whole range first (no access, no commit), then map the views over it; another thread may take part of it in between, so try again.
    int error = 0;
    for (int attempt = 0; attempt < 16 && !g_base; ++attempt) {
        // 2 MB more than the span, trimmed to a 2 MB aligned base: a huge page needs the same alignment in the file and in the address space.
        constexpr uint64_t kHuge = 0x200000;
        void* raw = mmap(nullptr, kSpan + kHuge, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (raw == MAP_FAILED) { error = errno; break; }
        auto* reserved = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(raw) + kHuge - 1) & ~uintptr_t(kHuge - 1));
        if (reserved != raw) munmap(raw, size_t(reserved - static_cast<uint8_t*>(raw)));
        munmap(reserved + kSpan, size_t(static_cast<uint8_t*>(raw) + kSpan + kHuge - (reserved + kSpan)));
        if (map_all(reserved, files)) g_base = reserved;
        else { error = errno; unmap_all(reserved); }
    }
    for (int file : files) close(file);   // the mappings keep the memory alive
    if (!g_base) give_up("4 GiB range", error);
}
uint8_t* guest_mem_base() { return g_base; }
}  // namespace uc
