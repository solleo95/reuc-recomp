// Linux: the executable's code (about 55 MB, most of it translated guest code with a flat profile) on 2 MB pages, so the instruction TLB covers it.
// The kernel gives file-backed code only 4 KB pages here (no READ_ONLY_THP_FOR_FS on the Deck), so the 2 MB aligned middle of the code segment is copied
// into anonymous memory, which THP backs with 2 MB pages, and moved over the original addresses: the same bytes at the same addresses, so code running
// in that range (this function included) continues unchanged. Any failure keeps the original mapping.
// macOS: nothing (no mremap, and signed code cannot be replaced by a copy); Apple Silicon's 16 KB pages already cover four times as much per TLB entry.
// Android: nothing either, the first object dl_iterate_phdr names is app_process64, not the game's library.
#if defined(__APPLE__) || defined(__ANDROID__)
namespace uc {
void text_to_huge_pages() {}
}  // namespace uc
#else
#include <cstdint>
#include <cstring>
#include <link.h>
#include <sys/mman.h>

namespace uc {
void text_to_huge_pages() {
    uintptr_t low = 0, high = 0;
    struct Range { uintptr_t* low; uintptr_t* high; } range{&low, &high};
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) {   // the first object is the executable: its executable segment
        auto* r = static_cast<Range*>(data);
        for (int i = 0; i < info->dlpi_phnum; ++i) if (info->dlpi_phdr[i].p_type == PT_LOAD && (info->dlpi_phdr[i].p_flags & PF_X)) {
            *r->low = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr; *r->high = *r->low + info->dlpi_phdr[i].p_memsz;
        }
        return 1; }, &range);
    constexpr uintptr_t kHuge = 0x200000;
    const uintptr_t start = (low + kHuge - 1) & ~(kHuge - 1), end = high & ~(kHuge - 1);
    if (!low || end <= start) return;
    const size_t size = end - start;
    void* raw = mmap(nullptr, size + kHuge, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return;
    auto* const first = static_cast<uint8_t*>(raw);
    auto* const copy = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(raw) + kHuge - 1) & ~(kHuge - 1));
    madvise(copy, size, MADV_HUGEPAGE);
    std::memcpy(copy, reinterpret_cast<const void*>(start), size);
    if (mprotect(copy, size, PROT_READ | PROT_EXEC) != 0 || mremap(copy, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, reinterpret_cast<void*>(start)) == MAP_FAILED) {
        munmap(raw, size + kHuge);
        return;
    }
    if (copy != first) munmap(first, size_t(copy - first));   // the unused ends of the over-allocation
    munmap(copy + size, size_t(first + size + kHuge - (copy + size)));
}
}  // namespace uc
#endif
