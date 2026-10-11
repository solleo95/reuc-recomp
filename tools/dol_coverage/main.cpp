#include "dol.h"
#include "ppc/insn.h"
#include "ppc/isa.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: dol_coverage <main.dol>\n"); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto d = uc::parse_dol(f);
    if (!d.ok) { std::fprintf(stderr, "invalid DOL: %s\n", d.error.c_str()); return 1; }
    size_t words = 0, zero = 0, bad = 0, oe_used = 0, unsup = 0;
    std::map<uint32_t, size_t> undecoded;  // key = primary<<16 | xo10
    std::map<std::string, size_t> counts;
    for (const auto& s : d.image.sections) {
        if (!s.is_text) continue;
        auto b = uc::section_bytes(f, s);
        for (size_t i = 0; i + 4 <= b.size(); i += 4) {
            uint32_t w = uint32_t(b[i]) << 24 | uint32_t(b[i + 1]) << 16 | uint32_t(b[i + 2]) << 8 | b[i + 3];
            ++words;
            if (w == 0) { ++zero; continue; }
            auto* o = ppc::decode(w);
            if (!o) { ++bad; ++undecoded[(w >> 26) << 16 | ((w >> 1) & 0x3FF)]; continue; }
            ++counts[o->name];
            if (o->kind == ppc::Kind::UNSUP) ++unsup;
            if (o->primary == 31 && o->sub == ppc::Sub::X9 && ppc::oe(w)) ++oe_used;
        }
    }
    std::printf("words %zu  zero %zu  undecoded %zu  unsupported-kind %zu  OE-set %zu\n", words, zero, bad, unsup, oe_used);
    for (auto& [k, v] : undecoded) std::printf("undecoded primary %u xo10 %u : %zu\n", k >> 16, k & 0xFFFF, v);
    std::printf("distinct decoded ops: %zu\n", counts.size());
    return 0;
}
