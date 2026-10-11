#include "check.h"
#include "dol.h"
#include "dol_builder.h"

int main() {
    // valid: one text section, entry, address, flags
    {
        auto f = make_dol({0x60000000, 0x4E800020});
        auto r = uc::parse_dol(f);
        CHECK(r.ok);
        CHECK(r.image.sections.size() == 1);
        CHECK(r.image.sections[0].is_text);
        CHECK(r.image.sections[0].address == 0x80003100);
        CHECK(r.image.sections[0].size == 8);
        CHECK(r.image.entry == 0x80003100);
        CHECK(uc::section_bytes(f, r.image.sections[0]).size() == 8);
    }
    // text + data
    {
        auto f = make_dol({0x60000000}, {1, 2, 3, 4, 5, 6, 7, 8});
        auto r = uc::parse_dol(f);
        CHECK(r.ok);
        CHECK(r.image.sections.size() == 2);
        CHECK(!r.image.sections[1].is_text);
        CHECK(r.image.sections[1].address == 0x80400000);
    }
    // review focus 1: truncated / garbage input must fail cleanly
    {
        std::vector<uint8_t> tiny(0x40, 0);
        CHECK(!uc::parse_dol(tiny).ok);
        CHECK(!uc::parse_dol({}).ok);
        std::vector<uint8_t> zero_header(0x100, 0);
        CHECK(!uc::parse_dol(zero_header).ok);  // no sections
        auto f = make_dol({0x60000000, 0x60000000});
        put32(f, 0x90, 0x1000);                  // size runs past EOF
        CHECK(!uc::parse_dol(f).ok);
        auto g = make_dol({0x60000000});
        put32(g, 0x48, 0x00001000);              // address below 0x80000000
        CHECK(!uc::parse_dol(g).ok);
    }
    return 0;
}
