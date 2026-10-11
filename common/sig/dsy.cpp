#include "sig/dsy.h"
#include <fstream>
#include <iterator>

namespace uc {

constexpr size_t kEntryBytes = 136;

static uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

bool parse_dsy(std::span<const uint8_t> b, std::vector<DsyEntry>& out, std::string& error) {
    out.clear();
    if (b.size() < 4) { error = "file smaller than the 4-byte header"; return false; }
    uint32_t count = le32(b.data());
    if (count > (b.size() - 4) / kEntryBytes) { error = "entry count does not fit in the file (truncated or not a .dsy)"; return false; }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = b.data() + 4 + size_t(i) * kEntryBytes;
        DsyEntry d{le32(e), le32(e + 4), {}};
        for (int k = 0; k < 127 && e[8 + k]; ++k) d.name.push_back(char(e[8 + k]));
        out.push_back(std::move(d));
    }
    return true;
}

bool load_dsy(const std::string& path, std::vector<DsyEntry>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "cannot open " + path; return false; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_dsy(bytes, out, error);
}

uint32_t dsy_step(uint32_t sum, uint32_t w) {
    uint32_t op = w & 0xFC000000u, op2 = 0, op3 = 0;
    uint32_t primary = op >> 26;
    switch (primary) {
        case 4:
            op2 = w & 0x3F;
            switch (op2) { case 0: case 8: case 16: case 21: case 22: op3 = w & 0x7C0; break; }
            break;
        case 7: case 8: case 10: case 11: case 12: case 13: case 14: case 15: op2 = w & 0x03FF0000; break;
        case 19: case 31: case 63: op2 = w & 0x7FF; break;
        case 59:
            op2 = w & 0x3F;
            if (op2 < 16) op3 = w & 0x7C0;
            break;
        default:
            if (primary >= 32 && primary <= 55) op2 = w & 0x03FF0000;
            break;
    }
    return ((sum << 17) | (sum >> 15)) ^ (op | op2 | op3);
}

}  // namespace uc
