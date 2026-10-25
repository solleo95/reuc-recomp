#pragma once
#include <cstddef>
#include <cstdint>
#include "rollback_memory.h"

namespace gx {
// Guest write barrier. Every guest store (and every native bulk write into guest RAM) records the current epoch in its 4 KiB page, so a cache
// holding a copy of guest memory can skip re-reading it when no store touched the pages since it last verified them. Written only by the thread
// holding the guest baton; the native code that reads it runs on that thread too, while no guest code executes.
inline constexpr uint32_t kWatchPages = 1u << 18;   // 1 GiB of guest address space / 4 KiB
inline uint32_t write_epoch = 2;
inline uint32_t page_epoch[kWatchPages];

inline void note_write(uint32_t address) { page_epoch[(address & 0x3FFFFFFFu) >> 12] = write_epoch; uc::rollback_note_write(address); }
inline void note_write_range(uint32_t address, size_t size) {
    if (!size) return;
    uc::rollback_note_range(address,size);
    const uint64_t first = (address & 0x3FFFFFFFu) >> 12, last = ((address & 0x3FFFFFFFu) + uint64_t(size) - 1) >> 12;
    for (uint64_t page = first; page <= last && page < kWatchPages; ++page) page_epoch[page] = write_epoch;
}
// "Verified now": every store after this call gets a larger epoch than the returned stamp.
inline uint32_t watch_stamp() { return write_epoch++; }
inline bool written_since(uint32_t address, size_t size, uint32_t stamp) {
    if (!size) return false;
    const uint64_t first = (address & 0x3FFFFFFFu) >> 12, last = ((address & 0x3FFFFFFFu) + uint64_t(size) - 1) >> 12;
    for (uint64_t page = first; page <= last; ++page) if (page >= kWatchPages || page_epoch[page] > stamp) return true;
    return false;
}
// The epoch counter is 32 bit; callers drop their stamps and call this long before it wraps.
inline bool watch_exhausted() { return write_epoch > 0xF0000000u; }
inline void watch_reset() { for (auto& epoch : page_epoch) epoch = 0; write_epoch = 2; }
}
