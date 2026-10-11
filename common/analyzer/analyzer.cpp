#include "analyzer/analyzer.h"
#include <algorithm>
#include <iterator>
#include <optional>

namespace uc {

const DolSection* Image::section(uint32_t a) const {
    for (const auto& s : dol->sections) if (a >= s.address && a - s.address < s.size) return &s;
    return nullptr;
}
bool Image::word(uint32_t a, uint32_t& out) const {
    auto* s = section(a);
    if (!s || (a & 3) || a - s->address + 4 > s->size) return false;
    const uint8_t* p = file.data() + s->file_offset + (a - s->address);
    out = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    return true;
}
bool Image::in_text(uint32_t a) const { auto* s = section(a); return s && s->is_text; }

const Function* Analysis::find_containing(uint32_t a) const {
    auto it = std::upper_bound(functions.begin(), functions.end(), a, [](uint32_t v, const Function& f) { return v < f.entry; });
    if (it == functions.begin()) return nullptr;
    --it;
    return a < it->end ? &*it : nullptr;
}

static bool is_terminator(uint32_t w) {
    return w == 0 || w == 0x4E800020u || w == 0x4E800420u || ((w >> 26) == 18 && !(w & 1));  // blr, bctr, b, padding
}

static std::vector<Function> make_functions(const Image& img, const std::set<uint32_t>& entries) {
    std::vector<Function> r;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const DolSection* s = img.section(*it);
        uint32_t sec_end = s->address + (s->size & ~3u);
        auto nx = std::next(it);
        uint32_t end = (nx != entries.end() && *nx < sec_end) ? *nx : sec_end;
        r.push_back({*it, end});
    }
    return r;
}

// True if the bctr at `bctr_pc` looks like a switch: `cmplwi rN,K ... lwzx rX,.. ; mtctr rX ; bctr` (lo_bound limits the backward search).
static bool switch_shape(const Image& img, uint32_t bctr_pc, uint32_t lo_bound) {
    uint32_t w = 0, mtctr_pc = 0, reg = 0, lw_pc = 0;
    for (int i = 1; i <= 6 && bctr_pc >= lo_bound + 4u * i; ++i) {
        uint32_t p = bctr_pc - 4u * i;
        if (img.word(p, w) && (w & 0xFC1FFFFFu) == 0x7C0903A6u) { mtctr_pc = p; reg = (w >> 21) & 31; break; }
    }
    if (!mtctr_pc) return false;
    for (int i = 1; i <= 8 && mtctr_pc >= lo_bound + 4u * i; ++i) {
        uint32_t p = mtctr_pc - 4u * i;
        if (img.word(p, w) && (w >> 26) == 31 && ((w >> 1) & 0x3FF) == 23 && ((w >> 21) & 31) == reg) { lw_pc = p; break; }
    }
    if (!lw_pc) return false;
    for (int i = 1; i <= 24 && lw_pc >= lo_bound + 4u * i; ++i)
        if (img.word(lw_pc - 4u * i, w) && (w >> 26) == 10) return true;
    return false;
}

// Finds `cmplwi rN,K ... lis/addi base ... slwi ... lwzx rX,rBase,rIdx ; mtctr rX ; bctr` and reads K+1 targets from the table.
static std::optional<JumpTable> find_table(const Image& img, const Function& f, uint32_t bctr_pc, bool& shaped) {
    shaped = switch_shape(img, bctr_pc, f.entry);
    if (!shaped) return std::nullopt;
    uint32_t w = 0;
    uint32_t mtctr_pc = 0, reg = 0;
    for (int i = 1; i <= 6; ++i) {
        if (bctr_pc < f.entry + 4u * i) break;
        uint32_t p = bctr_pc - 4u * i;
        if (img.word(p, w) && (w & 0xFC1FFFFFu) == 0x7C0903A6u) { mtctr_pc = p; reg = (w >> 21) & 31; break; }
    }
    if (!mtctr_pc) return std::nullopt;
    uint32_t lw_pc = 0, ra = 0, rb = 0;
    for (int i = 1; i <= 8; ++i) {
        if (mtctr_pc < f.entry + 4u * i) break;
        uint32_t p = mtctr_pc - 4u * i;
        if (img.word(p, w) && (w >> 26) == 31 && ((w >> 1) & 0x3FF) == 23 && ((w >> 21) & 31) == reg) {
            lw_pc = p; ra = (w >> 16) & 31; rb = (w >> 11) & 31; break;
        }
    }
    if (!lw_pc) return std::nullopt;
    shaped = true;
    uint32_t count = 0;
    for (int i = 1; i <= 24; ++i) {
        if (lw_pc < f.entry + 4u * i) break;
        if (img.word(lw_pc - 4u * i, w) && (w >> 26) == 10) { count = (w & 0xFFFF) + 1; break; }
    }
    if (count < 2 || count > 512) return std::nullopt;
    // Base address = (hi << 16) + lo where lo comes from `addi r,src,lo` / `ori r,r,lo` and hi from `lis src,hi`.
    // `src` is r itself (adjacent pair) or another register set earlier (hoisted by the compiler): then search the whole function.
    auto base_of = [&](uint32_t r, uint32_t& val) {
        bool have_lo = false; uint32_t lo = 0, hi_reg = r;
        int limit = 20;
        for (int i = 1; i <= limit; ++i) {
            if (lw_pc < f.entry + 4u * i) break;
            if (!img.word(lw_pc - 4u * i, w)) break;
            uint32_t prim = w >> 26, t = (w >> 21) & 31, a = (w >> 16) & 31;
            if (!have_lo && prim == 14 && t == r && a != 0) {
                have_lo = true; lo = uint32_t(int32_t(int16_t(w))); hi_reg = a;
                if (a != r) limit = 400;
            }
            else if (!have_lo && prim == 24 && t == r && a == r) { have_lo = true; lo = w & 0xFFFF; }
            else if (prim == 15 && t == hi_reg && a == 0) { val = ((w & 0xFFFF) << 16) + (have_lo ? lo : 0u); return true; }
        }
        return false;
    };
    for (uint32_t base_reg : {ra, rb}) {
        uint32_t base = 0;
        if (!base_of(base_reg, base)) continue;
        JumpTable jt{bctr_pc, {}};
        bool ok = true;
        for (uint32_t i = 0; i < count && ok; ++i) {
            uint32_t t = 0;
            ok = img.word(base + 4 * i, t) && !(t & 3) && t >= f.entry && t < f.end;
            jt.targets.push_back(t);
        }
        if (ok) return jt;
    }
    return std::nullopt;
}

Analysis analyze(const Image& img, const Config& cfg) {
    std::set<uint32_t> entries;
    auto add = [&](uint32_t a) { if (!(a & 3) && img.in_text(a) && !cfg.no_functions.count(a)) entries.insert(a); };
    add(img.dol->entry);
    for (uint32_t a : cfg.extra_entries) add(a);
    for (auto& [a, n] : cfg.hle) add(a);

    for (const auto& s : img.dol->sections) {
        if (!s.is_text) continue;
        uint32_t prev = 0;
        for (uint32_t pc = s.address; pc + 4 <= s.address + s.size; pc += 4) {
            uint32_t w = 0; img.word(pc, w);
            if ((w >> 26) == 18 && (w & 3) == 1) {  // bl
                uint32_t li = w & 0x03FFFFFCu; if (li & 0x02000000u) li |= 0xFC000000u;
                add(pc + li);
            }
            bool starts = pc == s.address || is_terminator(prev);
            if (starts && ((w & 0xFFFF0000u) == 0x94210000u || w == 0x7C0802A6u)) add(pc);  // stwu r1,-N(r1) | mflr r0
            prev = w;
        }
    }

    Analysis an;
    std::set<uint32_t> jt_targets;
    struct Found { std::vector<JumpTable> tables; std::vector<uint32_t> unresolved, tails; };
    auto find_tables = [&](const std::vector<Function>& fns) {
        Found r;
        r.tables = cfg.jump_tables;
        std::set<uint32_t> have;
        for (auto& j : r.tables) have.insert(j.bctr_addr);
        for (const auto& f : fns) {
            for (uint32_t pc = f.entry; pc < f.end; pc += 4) {
                uint32_t w = 0;
                if (!img.word(pc, w) || w != 0x4E800420u || have.count(pc)) continue;
                bool shaped = false;
                if (auto jt = find_table(img, f, pc, shaped)) { r.tables.push_back(*jt); have.insert(pc); }
                else if (shaped) r.unresolved.push_back(pc);
                else r.tails.push_back(pc);
            }
        }
        return r;
    };

    // Phase 1: functions from certain entries, so jump-table targets are known before data pointers are considered.
    auto fns = make_functions(img, entries);
    Found p1 = find_tables(fns);
    for (auto& j : p1.tables) for (uint32_t t : j.targets) jt_targets.insert(t);
    // Functions holding a switch whose table was not found: data pointers into their bodies are probably case labels.
    std::set<uint32_t> suspect;
    for (uint32_t pc : p1.unresolved) {
        auto it = std::upper_bound(fns.begin(), fns.end(), pc, [](uint32_t v, const Function& g) { return v < g.entry; });
        if (it != fns.begin()) suspect.insert(std::prev(it)->entry);
    }
    auto in_suspect_body = [&](uint32_t v) {
        auto it = std::upper_bound(fns.begin(), fns.end(), v, [](uint32_t a, const Function& g) { return a < g.entry; });
        if (it == fns.begin()) return false;
        const Function& g = *std::prev(it);
        return v < g.end && v != g.entry && suspect.count(g.entry) != 0;
    };

    // Phase 2: data words that point at code right after a terminator are function-pointer candidates (vtables, callbacks).
    for (const auto& s : img.dol->sections) {
        if (s.is_text) continue;
        for (uint32_t a = s.address; a + 4 <= s.address + s.size; a += 4) {
            uint32_t v = 0, pw = 0;
            if (!img.word(a, v) || (v & 3) || !img.in_text(v) || jt_targets.count(v) || in_suspect_body(v)) continue;
            if (!img.word(v - 4, pw) || !is_terminator(pw)) continue;
            // After a plain `b` the code is more likely a switch case label than a function: require a prologue there.
            bool after_branch = (pw >> 26) == 18;
            uint32_t first = 0; img.word(v, first);
            if (after_branch && (first & 0xFFFF0000u) != 0x94210000u && first != 0x7C0802A6u) continue;
            add(v);
        }
    }

    an.entries = entries;
    an.functions = make_functions(img, entries);
    Found fin = find_tables(an.functions);
    an.jump_tables = fin.tables;
    an.unresolved_bctr = fin.unresolved;
    an.indirect_tail = fin.tails;

    // Branches (any kind, linking or not) that leave their function and land inside another one make that address
    // a secondary entry of the other function.
    for (const auto& f : an.functions) {
        for (uint32_t pc = f.entry; pc < f.end; pc += 4) {
            uint32_t w = 0;
            if (!img.word(pc, w)) continue;
            uint32_t prim = w >> 26, t = 0;
            if (prim == 18) { int32_t li = int32_t(w << 6) >> 6 & ~3; t = (w & 2) ? uint32_t(li) : pc + uint32_t(li); }
            else if (prim == 16) { int32_t bd = int16_t(w & 0xFFFC); t = (w & 2) ? uint32_t(bd) : pc + uint32_t(bd); }
            else continue;
            if ((t & 3) || (t >= f.entry && t < f.end)) continue;
            const Function* g = an.find_containing(t);
            if (g && g->entry != t) an.secondary[g->entry].insert(t);
        }
    }
    return an;
}

}  // namespace uc
