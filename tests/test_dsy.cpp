#include "check.h"
#include "sig/dsy.h"
#include <cstring>

static void put32le(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }

int main() {
    // checksum: hand-computed. blr alone -> primary 19 keeps the low 11 bits: 0x4C000000 | 0x020
    CHECK(uc::dsy_step(0, 0x4E800020) == 0x4C000020);
    // li r3,1 ; blr -> addi keeps bits 16-25 (0x00600000): first sum 0x38600000; rotl17 = 0x000070C0; xor 0x4C000020
    uint32_t s = uc::dsy_step(0, 0x38600001);
    CHECK(s == 0x38600000);
    CHECK(uc::dsy_step(s, 0x4E800020) == 0x4C0070E0);
    // the displacement of a load (primary 32..55) is ignored, the register fields are kept
    CHECK(uc::dsy_step(0, 0x80010014) == uc::dsy_step(0, 0x80010028));

    // file format: u32 count + 136-byte entries
    std::vector<uint8_t> f;
    put32le(f, 2);
    for (int e = 0; e < 2; ++e) {
        put32le(f, 0x1000 + e); put32le(f, 40 + e);
        char name[128] = {};
        std::strcpy(name, e == 0 ? "OSInit" : "GXInit");
        f.insert(f.end(), name, name + 128);
    }
    std::vector<uc::DsyEntry> out; std::string err;
    CHECK(uc::parse_dsy(f, out, err));
    CHECK(out.size() == 2 && out[0].name == "OSInit" && out[1].checksum == 0x1001 && out[1].size == 41);
    // truncated file is rejected (Dolphin's loader would accept it)
    std::vector<uint8_t> cut(f.begin(), f.end() - 10);
    CHECK(!uc::parse_dsy(cut, out, err));
    // absurd count is rejected without allocating
    std::vector<uint8_t> huge; put32le(huge, 0x7FFFFFFF);
    CHECK(!uc::parse_dsy(huge, out, err));
    CHECK(!uc::parse_dsy({}, out, err));
    // name without terminator is cut at 127 characters
    std::vector<uint8_t> g; put32le(g, 1); put32le(g, 1); put32le(g, 4);
    g.insert(g.end(), 128, 'A');
    CHECK(uc::parse_dsy(g, out, err) && out[0].name.size() == 127);
    return 0;
}
