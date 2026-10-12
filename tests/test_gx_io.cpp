#include "check.h"
#include "mmio.h"
#include "ppc_fixture.h"
#include <array>
#include <vector>

namespace {
struct Write { uint32_t offset; int size; uint32_t value; };
struct FifoProbe : uc::MmioDevice {
    std::vector<Write> writes;
    uint32_t read(uint32_t, int) override { return 0; }
    void write(uint32_t off, int size, uint32_t value) override { writes.push_back({off, size, value}); }
} fifo;
uc::MmioBus bus;
void expect(std::initializer_list<Write> want) {
    CHECK(fifo.writes.size() == want.size());
    size_t i = 0;
    for (const auto& w : want) {
        CHECK(fifo.writes[i].offset == w.offset);
        CHECK(fifo.writes[i].size == w.size);
        CHECK(fifo.writes[i++].value == w.value);
    }
    fifo.writes.clear();
}
}

namespace ppc {
uint32_t mmio_read(uint32_t ea, int size) { return bus.read(ea, size); }
void mmio_write(uint32_t ea, int size, uint32_t value) { bus.write(ea, size, value); }
}

int main() {
    Fix f;
    auto& c = f.c;
    constexpr uint32_t pipe = 0xCC008000;
    bus.map(pipe, 0x100, &fifo, "GX vertex pipe");
    // Truncation and repeated pipe addresses preserve the original write widths.
    ppc::wr8(c, pipe, 0x1122); ppc::wr16(c, pipe, 0x334455); ppc::wr32(c, pipe, 0x66778899);
    expect({{0, 1, 0x22}, {0, 2, 0x4455}, {0, 4, 0x66778899}});
    c.r[3] = pipe; c.f[1] = 1.5;
    ppc::op_stfs(c, 1, 3, 0); expect({{0, 4, 0x3FC00000}});
    // Double stores emit high word then low word; second address is a pipe alias.
    c.f[1] = -2.25;
    ppc::op_stfd(c, 1, 3, 0); expect({{0, 4, 0xC0020000}, {4, 4, 0}});
    c.f[1] = 1.0; c.ps1[1] = -2.0; c.gqr[0] = 0;
    ppc::op_psq_st(c, 1, 3, 0, 0, 0);
    expect({{0, 4, 0x3F800000}, {4, 4, 0xC0000000}});
    ppc::op_psq_st(c, 1, 3, 0, 1, 0); expect({{0, 4, 0x3F800000}});
    // Quantized pairs route both values, including saturation and signed encoding.
    c.gqr[1] = 4 | (1 << 8); c.f[1] = 12.5; c.ps1[1] = 200.0;
    ppc::op_psq_st(c, 1, 3, 0, 0, 1); expect({{0, 1, 25}, {1, 1, 255}});
    c.gqr[1] = 7; c.f[1] = -1.0; c.ps1[1] = 1234.0;
    ppc::op_psq_st(c, 1, 3, 0, 0, 1); expect({{0, 2, 0xFFFF}, {2, 2, 1234}});
    c.r[4] = 8; c.f[1] = 3.0; c.ps1[1] = 4.0;
    ppc::op_psq_stux(c, 1, 3, 4, 0, 0);
    expect({{8, 4, 0x40400000}, {12, 4, 0x40800000}});
    CHECK(c.r[3] == pipe + 8);
    // Ordinary RAM stores must never reach the vertex sink.
    ppc::wrf32(c, 0x80001000, 1.5f); ppc::wrf64(c, 0x80001008, -2.25);
    CHECK(ppc::rd32(c, 0x80001000) == 0x3FC00000);
    CHECK(ppc::rd32(c, 0x80001008) == 0xC0020000 && ppc::rd32(c, 0x8000100C) == 0);
    CHECK(fifo.writes.empty());
    ppc::wr32(c, pipe + 0xFC, 0x12345678); expect({{0xFC, 4, 0x12345678}});
    ppc::wr8(c, pipe + 0x100, 1);
    CHECK(fifo.writes.empty() && bus.unmapped_count() == 1);
    return 0;
}
