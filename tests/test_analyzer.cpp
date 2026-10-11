#include "analyzer/analyzer.h"
#include "check.h"
#include "dol_builder.h"
#include "ppc_enc.h"
using namespace enc;

// f0 @ idx0..7: prologue, bl f1, epilogue.   f1 @ idx8..9: leaf, bl target.
// f2 @ idx10..25: switch via jump table (entry given by config).   f3 @ idx26..30: prologue after blr.
static std::vector<uint8_t> make(uint32_t table_entry2_override, Asm& a) {
    size_t call = 0;
    a.emit(stwu(1, -16, 1)); a.emit(mflr(0)); a.emit(stw(0, 20, 1));
    call = a.emit(0);
    a.emit(lwz(0, 20, 1)); a.emit(mtlr(0)); a.emit(addi(1, 1, 16)); a.emit(blr);
    uint32_t f1 = a.here(); a.emit(addi(3, 3, 1)); a.emit(blr);
    a.patch_bl(call, f1);
    a.emit(cmplwi(3, 2));
    size_t bgt_i = a.emit(0);
    a.emit(lis(4, 0x8040)); a.emit(addi(4, 4, 0)); a.emit(slwi(0, 3, 2)); a.emit(lwzx(0, 4, 0)); a.emit(mtctr(0)); a.emit(bctr);
    uint32_t c0 = a.here(); a.emit(li(3, 100)); a.emit(blr);
    uint32_t c1 = a.here(); a.emit(li(3, 200)); a.emit(blr);
    uint32_t c2 = a.here(); a.emit(li(3, 300)); a.emit(blr);
    uint32_t dflt = a.here(); a.emit(li(3, -1)); a.emit(blr);
    a.patch_bc(bgt_i, 12, 1, dflt);
    a.emit(stwu(1, -16, 1)); a.emit(mflr(0)); a.emit(mtlr(0)); a.emit(addi(1, 1, 16)); a.emit(blr);
    std::vector<uint8_t> data;
    be32(data, c0); be32(data, c1); be32(data, table_entry2_override ? table_entry2_override : c2);
    return make_dol(a.w, data);
}

int main() {
    {   // entry f2 (no bl reaches it) is supplied by config
        uc::Config cfg;
        Asm a; auto dol = make(0, a);
        auto parsed = uc::parse_dol(dol); CHECK(parsed.ok);
        uc::Image img{dol, &parsed.image};
        cfg.extra_entries.insert(a.addr(10));
        auto an = uc::analyze(img, cfg);
        CHECK(an.functions.size() == 4);
        CHECK(an.functions[0].entry == a.addr(0)  && an.functions[0].end == a.addr(8));
        CHECK(an.functions[1].entry == a.addr(8)  && an.functions[1].end == a.addr(10));
        CHECK(an.functions[2].entry == a.addr(10) && an.functions[2].end == a.addr(26));
        CHECK(an.functions[3].entry == a.addr(26) && an.functions[3].end == a.addr(31));
        CHECK(an.is_entry(a.addr(26)));                       // prologue after a blr
        CHECK(!an.is_entry(a.addr(18)));                      // jump-table targets are NOT function entries
        CHECK(an.jump_tables.size() == 1);
        CHECK(an.jump_tables[0].bctr_addr == a.addr(17));
        CHECK(an.jump_tables[0].targets.size() == 3);
        CHECK(an.jump_tables[0].targets[0] == a.addr(18) && an.jump_tables[0].targets[2] == a.addr(22));
        CHECK(an.unresolved_bctr.empty());
        CHECK(an.find_containing(a.addr(20))->entry == a.addr(10));
    }
    {   // review focus: a table entry pointing outside the function is rejected, bctr stays unresolved
        Asm a; auto dol = make(0x80003000, a);
        auto parsed = uc::parse_dol(dol);
        uc::Image img{dol, &parsed.image};
        uc::Config c2; c2.extra_entries.insert(a.addr(10));
        auto an = uc::analyze(img, c2);
        CHECK(an.jump_tables.empty());
        CHECK(an.unresolved_bctr.size() == 1 && an.unresolved_bctr[0] == a.addr(17));
    }
    {   // nofunc removes a detected entry; an explicit jumptable in the config resolves the bctr
        Asm a; auto dol = make(0x80003000, a);
        auto parsed = uc::parse_dol(dol);
        uc::Image img{dol, &parsed.image};
        uc::Config c3; c3.extra_entries.insert(a.addr(10)); c3.no_functions.insert(a.addr(26));
        c3.jump_tables.push_back({a.addr(17), {a.addr(18), a.addr(20), a.addr(22)}});
        auto an = uc::analyze(img, c3);
        CHECK(!an.is_entry(a.addr(26)));
        CHECK(an.jump_tables.size() == 1 && an.unresolved_bctr.empty());
    }
    {   // a virtual-call thunk is an indirect tail call, not a switch table; a branch into the middle of
        // another function makes that address a secondary entry of the function
        Asm a;
        size_t br = a.emit(0); a.emit(blr);
        uint32_t g = a.here(); a.emit(li(3, 1)); a.emit(li(3, 2)); a.emit(blr);
        uint32_t v = a.here(); a.emit(lwz(12, 0, 3)); a.emit(lwz(12, 8, 12)); a.emit(mtctr(12)); a.emit(bctr);
        a.patch_b(br, g + 4);
        auto dol = make_dol(a.w);
        auto parsed = uc::parse_dol(dol);
        uc::Image img{dol, &parsed.image};
        uc::Config c4; c4.extra_entries.insert(g); c4.extra_entries.insert(v);
        auto an = uc::analyze(img, c4);
        CHECK(an.functions.size() == 3);
        CHECK(an.secondary.count(g) && an.secondary.at(g).count(g + 4));
        CHECK(an.unresolved_bctr.empty());
        CHECK(an.indirect_tail.size() == 1 && an.indirect_tail[0] == a.addr(8));
    }
    {   // table base kept in another register, set far above the switch (hoisted by the compiler)
        Asm a;
        a.emit(lis(29, 0x8040));
        for (int i = 0; i < 30; ++i) a.emit(li(5, 0));
        a.emit(cmplwi(3, 1));
        size_t bgt_i = a.emit(0);
        a.emit(addi(4, 29, 0)); a.emit(slwi(0, 3, 2)); a.emit(lwzx(0, 4, 0)); a.emit(mtctr(0));
        size_t bctr_i = a.emit(bctr);
        uint32_t c0 = a.here(); a.emit(li(3, 10)); a.emit(blr);
        uint32_t c1 = a.here(); a.emit(li(3, 20)); a.emit(blr);
        uint32_t d = a.here(); a.emit(li(3, 0)); a.emit(blr);
        a.patch_bc(bgt_i, 12, 1, d);
        std::vector<uint8_t> data; be32(data, c0); be32(data, c1);
        auto dol = make_dol(a.w, data);
        auto parsed = uc::parse_dol(dol);
        uc::Image img{dol, &parsed.image};
        auto an = uc::analyze(img, uc::Config{});
        CHECK(an.jump_tables.size() == 1 && an.jump_tables[0].bctr_addr == a.addr(bctr_i));
        CHECK(an.unresolved_bctr.empty());
        CHECK(!an.is_entry(c0) && !an.is_entry(c1));
    }
    {   // a data word pointing at code right after an unconditional b (a case label), with no prologue, is not a function entry
        Asm a;
        a.emit(li(3, 1));
        size_t br = a.emit(0);
        uint32_t label = a.here(); a.emit(li(3, 5));
        uint32_t after = a.here(); a.emit(blr);
        a.patch_b(br, after);
        std::vector<uint8_t> data; be32(data, label);
        auto dol = make_dol(a.w, data);
        auto parsed = uc::parse_dol(dol);
        uc::Image img{dol, &parsed.image};
        auto an = uc::analyze(img, uc::Config{});
        CHECK(!an.is_entry(label));
    }
    {   // a switch whose table base cannot be found: its case labels (also referenced from data) must not become function entries,
        // and the bctr stays an unresolved switch; an lwzx-based adjustor thunk without a bound check is an indirect tail call
        Asm a;
        a.emit(cmplwi(3, 1));
        size_t bgt_i = a.emit(0);
        a.emit(lwz(4, 100, 13)); a.emit(slwi(0, 3, 2)); a.emit(lwzx(0, 4, 0)); a.emit(mtctr(0));
        size_t bctr_i = a.emit(bctr);
        uint32_t c0 = a.here(); a.emit(li(3, 10)); a.emit(blr);
        uint32_t c1 = a.here(); a.emit(li(3, 20)); a.emit(blr);
        uint32_t d = a.here(); a.emit(li(3, 0)); a.emit(blr);
        a.patch_bc(bgt_i, 12, 1, d);
        uint32_t adj = a.here();
        a.emit(lwz(12, 0, 3)); a.emit(lwzx(12, 3, 12)); a.emit(mtctr(12));
        size_t adj_bctr = a.emit(bctr);
        std::vector<uint8_t> data; be32(data, c0); be32(data, c1);
        auto dol = make_dol(a.w, data);
        auto parsed = uc::parse_dol(dol);
        uc::Image img{dol, &parsed.image};
        uc::Config c5; c5.extra_entries.insert(adj);
        auto an = uc::analyze(img, c5);
        CHECK(!an.is_entry(c0) && !an.is_entry(c1));
        CHECK(an.unresolved_bctr.size() == 1 && an.unresolved_bctr[0] == a.addr(bctr_i));
        CHECK(an.indirect_tail.size() == 1 && an.indirect_tail[0] == a.addr(adj_bctr));
    }
    {   // config text parsing
        uc::Config c; std::string err;
        CHECK(uc::parse_config("# comment\nentry 80001000\nnofunc 0x80002000\nhle 80003000 OSInit  # trailing\njumptable 80004000 80004010 80004020\n", c, err));
        CHECK(c.extra_entries.count(0x80001000) && c.no_functions.count(0x80002000));
        CHECK(c.hle.at(0x80003000) == "OSInit");
        CHECK(c.jump_tables.size() == 1 && c.jump_tables[0].targets.size() == 2);
        uc::Config bad;
        CHECK(!uc::parse_config("entry 80001000\nbogus 1\n", bad, err));
        CHECK(err.find("line 2") != std::string::npos);
        CHECK(!uc::parse_config("hle 80001000 bad-name\n", bad, err));
        CHECK(!uc::parse_config("entry zz\n", bad, err));
    }
    return 0;
}
