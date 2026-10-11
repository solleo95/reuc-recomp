#include "dol.h"

namespace uc {

static uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

DolResult parse_dol(std::span<const uint8_t> f) {
    DolResult r;
    if (f.size() < 0x100) { r.error = "file is smaller than the 0x100-byte DOL header"; return r; }
    for (int i = 0; i < 18; ++i) {  // 7 text + 11 data
        uint32_t off = be32(&f[i * 4]);
        uint32_t addr = be32(&f[0x48 + i * 4]);
        uint32_t size = be32(&f[0x90 + i * 4]);
        if (size == 0) continue;
        if (uint64_t(off) + size > f.size()) {
            r.error = "section " + std::to_string(i) + " extends past end of file";
            return r;
        }
        if (addr < 0x80000000u) {
            r.error = "section " + std::to_string(i) + " address is below 0x80000000";
            return r;
        }
        r.image.sections.push_back({off, addr, size, i < 7});
    }
    if (r.image.sections.empty()) { r.error = "DOL has no non-empty sections"; return r; }
    r.image.bss_address = be32(&f[0xD8]);
    r.image.bss_size = be32(&f[0xDC]);
    r.image.entry = be32(&f[0xE0]);
    r.ok = true;
    return r;
}

std::span<const uint8_t> section_bytes(std::span<const uint8_t> file, const DolSection& s) {
    return file.subspan(s.file_offset, s.size);
}

}  // namespace uc
