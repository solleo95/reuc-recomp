#include "mmio.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace uc {

uint32_t RegFile::read(uint32_t off, int size) {
    if (off + size > bytes.size()) return 0;
    uint32_t v = 0;
    for (int i = 0; i < size; ++i) v = v << 8 | bytes[off + i];
    return v;
}
void RegFile::write(uint32_t off, int size, uint32_t v) {
    if (off + size > bytes.size()) return;
    for (int i = size - 1; i >= 0; --i, v >>= 8) bytes[off + i] = uint8_t(v);
}

void MmioBus::map(uint32_t base, uint32_t size, MmioDevice* dev, std::string name) {
    for (const auto& r : ranges_)
        if (base < r.base + r.size && r.base < base + size) throw std::invalid_argument("MMIO range overlaps " + r.name);
    ranges_.push_back({base, size, dev, std::move(name)});
    std::sort(ranges_.begin(), ranges_.end(), [](const Range& a, const Range& b) { return a.base < b.base; });
}

const MmioBus::Range* MmioBus::find(uint32_t ea) const {
    auto it = std::upper_bound(ranges_.begin(), ranges_.end(), ea, [](uint32_t v, const Range& r) { return v < r.base; });
    if (it == ranges_.begin()) return nullptr;
    --it;
    return ea - it->base < it->size ? &*it : nullptr;
}

void MmioBus::report(const char* what, uint32_t ea, int size) {
    ++unmapped_;
    if (seen_.size() < 64 && seen_.insert({ea, size}).second) std::fprintf(stderr, "mmio: unmapped %s %08X size %d\n", what, ea, size);
}

uint32_t MmioBus::read(uint32_t ea, int size) {
    if (const Range* r = find(ea)) return r->dev->read(ea - r->base, size);
    report("read", ea, size);
    return 0;
}
void MmioBus::write(uint32_t ea, int size, uint32_t v) {
    if (const Range* r = find(ea)) { r->dev->write(ea - r->base, size, v); return; }
    report("write", ea, size);
}

MmioBus& mmio_bus() { static MmioBus b; return b; }
}  // namespace uc
