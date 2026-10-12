#include "ppc/format.h"
#include "ppc/insn.h"
#include "ppc/isa.h"
#include <cstdio>

namespace ppc {

static std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%08X", v); return b; }
static std::string r(uint32_t n) { return "r" + std::to_string(n); }
static std::string f(uint32_t n) { return "f" + std::to_string(n); }

std::string format_insn(uint32_t pc, uint32_t w) {
    const OpInfo* op = decode(w);
    if (!op) { char b[32]; std::snprintf(b, sizeof b, ".long 0x%08X", w); return b; }
    std::string name = op->name;
    if (!name.empty() && name.back() == '_') name.back() = '.';
    if (op->rc && rc(w)) name += ".";
    bool fp = (op->primary >= 48 && op->primary <= 55) ||
              (op->primary == 31 && (op->xo == 535 || op->xo == 567 || op->xo == 599 || op->xo == 631 || op->xo == 663 || op->xo == 695 || op->xo == 727 || op->xo == 759 || op->xo == 983));
    auto rt = [&](uint32_t n) { return fp ? f(n) : r(n); };
    std::string o;
    switch (op->kind) {
        case Kind::NONE: return name;
        case Kind::RRR:
            if (op->primary == 31 && (name.rfind("and", 0) == 0 || name.rfind("or", 0) == 0 || name.rfind("xor", 0) == 0 || name.rfind("nor", 0) == 0 ||
                                      name.rfind("nand", 0) == 0 || name.rfind("eqv", 0) == 0 || name.rfind("slw", 0) == 0 || name.rfind("srw", 0) == 0 ||
                                      name.rfind("sraw", 0) == 0 || name.rfind("srawi", 0) == 0)) {
                // logical and shift forms: the destination is rA, the sources are rS (t field) and rB (or the shift amount)
                o = r(a(w)) + "," + r(t(w)) + "," + (name.rfind("srawi", 0) == 0 ? std::to_string(b(w)) : r(b(w)));
            } else {
                o = rt(t(w)) + "," + r(a(w)) + "," + r(b(w));
            }
            break;
        case Kind::RRS:
            if (op->primary >= 32 && op->primary <= 55) o = rt(t(w)) + "," + std::to_string(simm(w)) + "(" + r(a(w)) + ")";
            else o = r(t(w)) + "," + r(a(w)) + "," + std::to_string(simm(w));
            break;
        case Kind::RRU: o = r(a(w)) + "," + r(t(w)) + "," + std::to_string(uimm(w)); break;
        case Kind::RR: o = r(a(w)) + "," + r(t(w)); break;
        case Kind::AB: o = r(a(w)) + "," + r(b(w)); break;
        case Kind::RB: o = f(t(w)) + "," + f(b(w)); break;
        case Kind::R: o = r(t(w)); break;
        case Kind::CRF: o = "cr" + std::to_string(crfd(w)); break;
        case Kind::RRM: o = r(a(w)) + "," + r(t(w)) + "," + std::to_string(sh(w)) + "," + std::to_string(mb(w)) + "," + std::to_string(me(w)); break;
        case Kind::RRRM: o = r(a(w)) + "," + r(t(w)) + "," + r(b(w)) + "," + std::to_string(mb(w)) + "," + std::to_string(me(w)); break;
        case Kind::CMP: o = "cr" + std::to_string(crfd(w)) + "," + r(a(w)) + "," + r(b(w)); break;
        case Kind::CMPS: o = "cr" + std::to_string(crfd(w)) + "," + r(a(w)) + "," + std::to_string(simm(w)); break;
        case Kind::CMPU: o = "cr" + std::to_string(crfd(w)) + "," + r(a(w)) + "," + std::to_string(uimm(w)); break;
        case Kind::CRB: o = std::to_string(t(w)) + "," + std::to_string(a(w)) + "," + std::to_string(b(w)); break;
        case Kind::MCRF: o = "cr" + std::to_string(crfd(w)) + ",cr" + std::to_string(crfs(w)); break;
        case Kind::SPR:
            if (name == "mfspr" || name == "mtspr") {
                uint32_t s = spr(w);
                const char* nm = s == 8 ? "lr" : s == 9 ? "ctr" : s == 1 ? "xer" : nullptr;
                if (nm) return (name == "mfspr" ? "mf" : "mt") + std::string(nm) + " " + r(t(w));
                o = name == "mfspr" ? r(t(w)) + "," + std::to_string(s) : std::to_string(s) + "," + r(t(w));
            } else {
                o = r(t(w)) + "," + std::to_string(spr(w));
            }
            break;
        case Kind::MTCRF: o = std::to_string(crm(w)) + "," + r(t(w)); break;
        case Kind::RRRR: o = f(t(w)) + "," + f(a(w)) + "," + f(b(w)) + "," + f(c(w)); break;
        case Kind::RAC: o = f(t(w)) + "," + f(a(w)) + "," + f(c(w)); break;
        case Kind::PSQ: o = f(t(w)) + "," + std::to_string(int32_t(w << 20) >> 20) + "(" + r(a(w)) + ")," + std::to_string((w >> 15) & 1) + "," + std::to_string((w >> 12) & 7); break;
        case Kind::PSQX: o = f(t(w)) + "," + r(a(w)) + "," + r(b(w)) + "," + std::to_string((w >> 10) & 1) + "," + std::to_string((w >> 7) & 7); break;
        case Kind::BR: {
            int32_t d = li(w);
            return std::string(lk(w) ? "bl " : "b ") + hex(aa(w) ? uint32_t(d) : pc + uint32_t(d));
        }
        case Kind::BC: {
            int32_t d = bd(w);
            return std::string(lk(w) ? "bcl " : "bc ") + std::to_string(bo(w)) + "," + std::to_string(bi(w)) + "," + hex(aa(w) ? uint32_t(d) : pc + uint32_t(d));
        }
        case Kind::BCLR: return w == 0x4E800020u ? std::string("blr") : (lk(w) ? "bclrl " : "bclr ") + std::to_string(bo(w)) + "," + std::to_string(bi(w));
        case Kind::BCCTR:
            return w == 0x4E800420u ? std::string("bctr") : w == 0x4E800421u ? std::string("bctrl") : (lk(w) ? "bcctrl " : "bcctr ") + std::to_string(bo(w)) + "," + std::to_string(bi(w));
        case Kind::SC: return "sc";
        default: return name;
    }
    return name + " " + o;
}

}  // namespace ppc
