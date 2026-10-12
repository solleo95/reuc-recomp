#include "check.h"
#include "mmio.h"
#include <stdexcept>

struct Probe : uc::MmioDevice {
    uint32_t last_off = 0xFFFFFFFF, last_val = 0; int last_size = 0;
    uint32_t read(uint32_t off, int size) override { last_off = off; last_size = size; return 0x11223344u >> (32 - 8 * size); }
    void write(uint32_t off, int size, uint32_t v) override { last_off = off; last_size = size; last_val = v; }
};

int main() {
    uc::MmioBus bus;
    Probe p; uc::RegFile regs(0x100);
    bus.map(0xCC002000, 0x100, &regs, "VI");
    bus.map(0xCC008000, 0x4, &p, "FIFO");
    // register file: big-endian, read-back, mixed widths
    bus.write(0xCC002010, 4, 0xAABBCCDD);
    CHECK(bus.read(0xCC002010, 4) == 0xAABBCCDD);
    CHECK(bus.read(0xCC002010, 2) == 0xAABB && bus.read(0xCC002012, 2) == 0xCCDD);
    CHECK(bus.read(0xCC002013, 1) == 0xDD);
    bus.write(0xCC002011, 1, 0x55);
    CHECK(bus.read(0xCC002010, 4) == 0xAA55CCDD);
    // routing and offsets
    bus.write(0xCC008000, 4, 0x1234);
    CHECK(p.last_off == 0 && p.last_val == 0x1234 && p.last_size == 4);
    CHECK(bus.read(0xCC008000, 2) == 0x1122);
    // boundaries: first byte past a range is unmapped
    CHECK(bus.unmapped_count() == 0);
    CHECK(bus.read(0xCC002100, 4) == 0);
    CHECK(bus.read(0xCC001FFC, 4) == 0);
    CHECK(bus.unmapped_count() == 2);
    bus.write(0xCC0FFFFC, 4, 1);   // unmapped write is counted, not fatal
    CHECK(bus.unmapped_count() == 3);
    // overlap is rejected
    bool threw = false;
    try { bus.map(0xCC0020F0, 0x20, &p, "overlap"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    return 0;
}
