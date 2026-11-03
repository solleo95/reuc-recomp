#include "translator/emit.h"
#include "ppc/insn.h"
#include "ppc/isa.h"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <sstream>

namespace uc {

static std::string hex8(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08X", v); return b; }
static std::string num(long long v) { return std::to_string(v); }
static std::string fn_name(uint32_t a) { return "fn_" + hex8(a); }
static std::string sec_name(uint32_t owner, uint32_t t) { return "fn_" + hex8(owner) + "_" + hex8(t); }

void add_resume_entries(Program& p) {
    for (const auto& f:p.an.functions) {
        if(p.cfg.hle.count(f.entry)) continue;
        for(uint32_t pc=f.entry;pc+4<f.end;pc+=4) {
            uint32_t word=0;
            if(!p.img.word(pc,word) || !ppc::lk(word)) continue;
            const auto* op=ppc::decode(word);
            if(op && (op->kind==ppc::Kind::BR || op->kind==ppc::Kind::BC ||
                      op->kind==ppc::Kind::BCLR || op->kind==ppc::Kind::BCCTR))
                p.an.secondary[f.entry].insert(pc+4);
        }
    }
}

static std::string callee(const Program& p, uint32_t target) {
    auto it = p.cfg.hle.find(target);
    return it != p.cfg.hle.end() ? "uc::invoke_hle<hle_" + it->second + ">" : fn_name(target);
}

static std::string trap_str(uint32_t pc, const std::string& what) { return "ppc::trap(0x" + hex8(pc) + ", \"" + what + "\");"; }

static std::string op_args(const ppc::OpInfo& op, uint32_t w) {
    using ppc::Kind;
    std::ostringstream o;
    auto n = [](auto v) { return std::to_string(static_cast<long long>(v)); };
    switch (op.kind) {
        case Kind::NONE: break;
        case Kind::RRR: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)); break;
        case Kind::RRS: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::simm(w)); break;
        case Kind::RRU: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::uimm(w)) << "u"; break;
        case Kind::RR: o << n(ppc::t(w)) << ", " << n(ppc::a(w)); break;
        case Kind::AB: o << n(ppc::a(w)) << ", " << n(ppc::b(w)); break;
        case Kind::RB: o << n(ppc::t(w)) << ", " << n(ppc::b(w)); break;
        case Kind::R: o << n(ppc::t(w)); break;
        case Kind::CRF: o << n(ppc::crfd(w)); break;
        case Kind::RRM: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::sh(w)) << ", " << n(ppc::mb(w)) << ", " << n(ppc::me(w)); break;
        case Kind::RRRM: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)) << ", " << n(ppc::mb(w)) << ", " << n(ppc::me(w)); break;
        case Kind::CMP: o << n(ppc::crfd(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)); break;
        case Kind::CMPS: o << n(ppc::crfd(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::simm(w)); break;
        case Kind::CMPU: o << n(ppc::crfd(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::uimm(w)) << "u"; break;
        case Kind::CRB: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)); break;
        case Kind::MCRF:
            if (std::string(op.name) == "mtfsfi") o << n(ppc::crfd(w)) << ", " << n((w >> 12) & 0xF);
            else o << n(ppc::crfd(w)) << ", " << n(ppc::crfs(w));
            break;
        case Kind::SPR: o << n(ppc::t(w)) << ", " << n(ppc::spr(w)); break;   // mfspr, mtspr, mftb share the swapped 10-bit field
        case Kind::MTCRF:
            if (std::string(op.name) == "mtfsf") o << n((w >> 17) & 0xFF) << ", " << n(ppc::b(w));
            else o << n(ppc::crm(w)) << ", " << n(ppc::t(w));
            break;
        case Kind::RRRR: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)) << ", " << n(ppc::c(w)); break;
        case Kind::RAC: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::c(w)); break;
        case Kind::PSQ: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(int32_t(w << 20) >> 20) << ", " << n((w >> 15) & 1) << ", " << n((w >> 12) & 7); break;
        case Kind::PSQX: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)) << ", " << n((w >> 10) & 1) << ", " << n((w >> 7) & 7); break;
        default: break;
    }
    return o.str();
}

// ---- control flow ----
struct Target { enum Class { Internal, Entry, Secondary, Outside } kind; uint32_t owner; };
static Target classify(const Program& p, const Function& f, uint32_t t) {
    if (t >= f.entry && t < f.end) return {Target::Internal, 0};
    if (p.an.is_entry(t)) return {Target::Entry, 0};
    if (const Function* g = p.an.find_containing(t)) {
        auto it = p.an.secondary.find(g->entry);
        if (!p.cfg.hle.count(g->entry) && it != p.an.secondary.end() && it->second.count(t)) return {Target::Secondary, g->entry};
    }
    return {Target::Outside, 0};
}

static std::string cond_expr(uint32_t bo, uint32_t bi, std::string& pre) {
    std::string e;
    if (!(bo & 4)) {
        pre += "--c.ctr; ";
        e = (bo & 2) ? "c.ctr == 0" : "c.ctr != 0";
    }
    if (!(bo & 16)) {
        std::string cr = "ppc::crbit(c, " + num(bi) + ") == " + ((bo & 8) ? "true" : "false");
        e = e.empty() ? cr : e + " && " + cr;
    }
    return e.empty() ? "true" : e;
}

static std::string branch_action(const Program& p, const Function& f, uint32_t pc, uint32_t target, bool lk, EmitStats& st) {
    Target tk = classify(p, f, target);
    if (tk.kind == Target::Internal && !lk) return std::string(target <= pc ? "ppc::poll(c); " : "") + "goto L_" + hex8(target) + ";";   // a loop is a safe point
    if (tk.kind == Target::Outside || (tk.kind == Target::Internal && lk && target != f.entry)) {
        ++st.traps_midfunc;
        return trap_str(pc, "branch target " + hex8(target) + " is not inside any translated function");
    }
    std::string fn = tk.kind == Target::Secondary ? sec_name(tk.owner, target) : callee(p, target);
    if (lk) return "c.lr = 0x" + hex8(pc + 4) + "u; " + fn + "(c);";
    return fn + "(c); return;";
}

// Branch condition from a compare that was not written to CR: bit 0..3 of BI = LT GT EQ SO of that compare.
static std::string fused_cond(const FusedCmp& cmp, uint32_t bo, uint32_t bi) {
    const unsigned bit = bi & 3;
    std::string e;
    if (cmp.equal_op) e = "(" + cmp.lhs + " " + cmp.equal_op + " " + cmp.rhs + ")";   // false for a NaN operand, as the CR the instruction pair would build
    else if (bit == 3) e = cmp.fp ? "(" + cmp.lhs + " != " + cmp.lhs + " || " + cmp.rhs + " != " + cmp.rhs + ")" : "c.xer_so != 0";
    else e = "(" + cmp.lhs + (bit == 0 ? " < " : bit == 1 ? " > " : " == ") + cmp.rhs + ")";
    return (bo & 8) ? e : "!(" + e + ")";
}

std::string emit_insn(const Program& p, const Function& f, uint32_t pc, uint32_t w, EmitStats& st, const CrHints* hints) {
    ++st.instructions;
    const ppc::OpInfo* op = ppc::decode(w);
    if (!op) {
        ++st.invalid;
        return trap_str(pc, "illegal or data word " + hex8(w));
    }
    using ppc::Kind;
    std::string name = op->name;
    if ((name == "twi" || name == "tw") && ppc::t(w) == 31) return trap_str(pc, "unconditional trap");
    if (op->sub == ppc::Sub::X9 && ppc::oe(w)) { ++st.oe_traps; return trap_str(pc, "OE (overflow-enabled) form of " + name + " is not implemented"); }
    if (op->kind == Kind::UNSUP) { ++st.unsupported; return trap_str(pc, "unsupported instruction " + name); }

    switch (op->kind) {
        case Kind::SC: return "uc::hle_sc(c, 0x" + hex8(pc) + "u);";
        case Kind::BR: {
            int32_t li = ppc::li(w);
            uint32_t target = ppc::aa(w) ? uint32_t(li) : pc + uint32_t(li);
            return branch_action(p, f, pc, target, ppc::lk(w), st);
        }
        case Kind::BC: {
            uint32_t bo = ppc::bo(w), bi = ppc::bi(w);
            uint32_t target = ppc::aa(w) ? uint32_t(ppc::bd(w)) : pc + uint32_t(ppc::bd(w));
            std::string pre;
            std::string cond = hints && hints->fused ? fused_cond(*hints->fused, bo, bi) : cond_expr(bo, bi, pre);
            return "{ " + pre + "if (" + cond + ") { " + branch_action(p, f, pc, target, ppc::lk(w), st) + " } }";
        }
        case Kind::BCLR: {
            uint32_t bo = ppc::bo(w), bi = ppc::bi(w);
            std::string pre;
            std::string cond = hints && hints->fused ? fused_cond(*hints->fused, bo, bi) : cond_expr(bo, bi, pre);
            std::string act = ppc::lk(w) ? "{ uint32_t t_ = c.lr; c.lr = 0x" + hex8(pc + 4) + "u; uc::call_indirect(c, t_); }" : "return;";
            if (cond == "true" && pre.empty()) return act;
            return "{ " + pre + "if (" + cond + ") { " + act + " } }";
        }
        case Kind::BCCTR: {
            uint32_t bo = ppc::bo(w), bi = ppc::bi(w);
            std::string pre;
            std::string cond = cond_expr(bo | 4, bi, pre);   // bcctr never decrements CTR
            if (ppc::lk(w)) {
                std::string act = "c.lr = 0x" + hex8(pc + 4) + "u; uc::call_indirect(c, c.ctr);";
                return cond == "true" ? act : "if (" + cond + ") { " + act + " }";
            }
            for (const auto& jt : p.an.jump_tables) {
                if (jt.bctr_addr != pc) continue;
                std::set<uint32_t> seen;
                std::string s = "switch (c.ctr) { ";
                for (uint32_t t : jt.targets) if (seen.insert(t).second) s += "case 0x" + hex8(t) + "u: goto L_" + hex8(t) + "; ";
                s += "default: " + trap_str(pc, "jump table index out of range") + " }";
                return s;
            }
            if (std::find(p.an.unresolved_bctr.begin(), p.an.unresolved_bctr.end(), pc) != p.an.unresolved_bctr.end()) ++st.unresolved_bctr;
            else ++st.indirect_tail;
            std::string act = "uc::call_indirect(c, c.ctr); return;";
            return cond == "true" ? "{ " + act + " }" : "if (" + cond + ") { " + act + " }";
        }
        default: break;
    }
    std::string a = op_args(*op, w);
    if (op->rc) a += (a.empty() ? "" : ", ") + std::string(ppc::rc(w) && !(hints && hints->drop_rc) ? "true" : "false");
    return "ppc::op_" + name + "(c" + (a.empty() ? "" : ", " + a) + ");";
}

static bool is_terminator_word(uint32_t w) { return w == 0x4E800020u || w == 0x4E800420u || ((w >> 26) == 18 && !(w & 1)); }

// ---- condition-register liveness ----
// A CR field write is dead when no path reads the field before another write replaces it. Backward dataflow over each function's
// instruction graph; what callers read after a return and what callees read on entry come from whole-program summaries, so no calling
// convention is assumed: a field is kept as long as any call site reads it after the call.
namespace {
constexpr uint8_t kAllCr = 0xFF;
uint8_t fld(uint32_t f) { return uint8_t(1u << (f & 7)); }
bool fp_name(const std::string& n) { return n[0] == 'f' || n == "mffs" || n.compare(0, 3, "ps_") == 0 || n.compare(0, 4, "mtfs") == 0; }

// The register an integer record-form instruction compares with zero (cr0 = sign of it), -1 for forms this does not know.
int rc_dest(const std::string& n, uint32_t w) {
    static const std::set<std::string> arithmetic{"add", "addc", "adde", "addze", "addme", "subf", "subfc", "subfe", "subfze", "subfme", "neg", "mullw", "mulhw", "mulhwu", "divw", "divwu"};
    static const std::set<std::string> logical{"and", "andc", "or", "orc", "xor", "nor", "nand", "eqv", "slw", "srw", "sraw", "srawi", "cntlzw", "extsb", "extsh", "rlwinm", "rlwnm", "rlwimi"};
    if (arithmetic.count(n)) return int(ppc::t(w));   // rD
    if (logical.count(n)) return int(ppc::a(w));      // rA
    return -1;
}

struct CrEffect { uint8_t use = 0, def = 0; };   // fields read, fields fully overwritten
CrEffect cr_effect(const ppc::OpInfo& op, uint32_t w) {
    using ppc::Kind;
    CrEffect e;
    const std::string n = op.name;
    switch (op.kind) {
        case Kind::CMP: case Kind::CMPS: case Kind::CMPU: case Kind::CRF: e.def = fld(ppc::crfd(w)); return e;   // cmp*, fcmp*, ps_cmp*, mcrxr
        case Kind::MCRF:
            if (n == "mtfsfi") { if (ppc::rc(w)) e.def = fld(1); }
            else if (n == "mcrf") { e.use = fld(ppc::crfs(w)); e.def = fld(ppc::crfd(w)); }
            else e.def = fld(ppc::crfd(w));   // mcrfs
            return e;
        case Kind::CRB: e.use = uint8_t(fld(ppc::t(w) >> 2) | fld(ppc::a(w) >> 2) | fld(ppc::b(w) >> 2)); return e;   // the target field is read-modify-write
        case Kind::MTCRF:
            if (n == "mtcrf") { for (unsigned i = 0; i < 8; ++i) if (ppc::crm(w) & (0x80u >> i)) e.def |= fld(i); }
            else if (ppc::rc(w)) e.def = fld(1);   // mtfsf
            return e;
        default: break;
    }
    if (n == "mfcr") { e.use = kAllCr; return e; }
    if (op.rc && ppc::rc(w)) e.def |= fld(fp_name(n) ? 1 : 0);          // record forms: cr0 for integer ops, cr1 for floating point
    else if (!op.rc && n.back() == '_') e.def |= fld(0);                // andi. addic. stwcx. ... always record into cr0
    return e;
}

struct Callee {
    enum Kind : uint8_t { None, Direct, Hle, Indirect } kind = None;
    uint32_t entry = 0;   // address whose entry liveness the call reads
    uint32_t fn = 0;      // function whose return the call joins
};
Callee resolve_callee(const Program& p, const Function& f, uint32_t target) {
    if (p.cfg.hle.count(target)) return {Callee::Hle, target, target};
    const Target tk = classify(p, f, target);
    return {Callee::Direct, target, tk.kind == Target::Secondary ? tk.owner : target};
}

struct CrNode {
    uint8_t use = 0, def = 0;
    bool fall = true;                  // execution can continue at the next instruction (also after a call returns)
    enum Taken : uint8_t { NoJump, Internal, Return, Tail, IndirectTail } taken = NoJump;
    int target = -1;                   // Internal: instruction index
    Callee call;                       // a call made by the instruction
    Callee tail;                       // Tail: the function it jumps to
    std::vector<int> table;            // jump table targets
};

std::vector<CrNode> cr_nodes(const Program& p, const Function& f) {
    using ppc::Kind;
    const size_t n = (f.end - f.entry) / 4;
    std::vector<CrNode> nodes(n);
    auto index = [&](uint32_t t) { return int((t - f.entry) / 4); };
    for (size_t i = 0; i < n; ++i) {
        const uint32_t pc = f.entry + uint32_t(i) * 4;
        uint32_t w = 0;
        CrNode& nd = nodes[i];
        const ppc::OpInfo* op = p.img.word(pc, w) ? ppc::decode(w) : nullptr;
        if (!op) { nd.fall = false; continue; }   // emitted as a trap
        const std::string name = op->name;
        if (((name == "twi" || name == "tw") && ppc::t(w) == 31) || (op->sub == ppc::Sub::X9 && ppc::oe(w)) || op->kind == Kind::UNSUP) { nd.fall = false; continue; }
        const CrEffect fx = cr_effect(*op, w);
        nd.use = fx.use; nd.def = fx.def;
        if (name == "mfcr") {
            // mfcr rT; rlwinm rT,rT,sh,mb,me: the whole CR word dies in the rotate and mask, so only the fields under the mask are read
            uint32_t next = 0;
            const ppc::OpInfo* nop = p.img.word(pc + 4, next) ? ppc::decode(next) : nullptr;
            if (nop && std::string(nop->name) == "rlwinm" && ppc::t(next) == ppc::t(w) && ppc::a(next) == ppc::t(w)) {
                const uint32_t mb = ppc::mb(next), me = ppc::me(next);
                const uint32_t mask = mb <= me ? (0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me)) : ((0xFFFFFFFFu >> mb) | (0xFFFFFFFFu << (31 - me)));
                const uint32_t sh = ppc::sh(next) & 31;
                const uint32_t bits = sh ? (mask >> sh) | (mask << (32 - sh)) : mask;   // the CR word bits that survive the rotate and mask
                uint8_t used = 0;
                for (unsigned i = 0; i < 8; ++i) if (bits & (0xFu << (28 - 4 * i))) used |= fld(i);
                nd.use = used;
            }
        }
        switch (op->kind) {
            case Kind::SC: nd.call = {Callee::Hle, 0, 0}; break;
            case Kind::BR: case Kind::BC: {
                const bool bc = op->kind == Kind::BC;
                const uint32_t bo = bc ? ppc::bo(w) : 20, bi = bc ? ppc::bi(w) : 0;
                const int32_t disp = bc ? ppc::bd(w) : ppc::li(w);
                const uint32_t target = ppc::aa(w) ? uint32_t(disp) : pc + uint32_t(disp);
                const Target tk = classify(p, f, target);
                if (bc && !(bo & 16)) nd.use |= fld(bi >> 2);
                if (ppc::lk(w)) {
                    if (!(tk.kind == Target::Outside || (tk.kind == Target::Internal && target != f.entry))) nd.call = resolve_callee(p, f, target);
                    nd.fall = true;   // a call continues after it returns; the other path of a conditional call skips it
                } else {
                    if (tk.kind == Target::Internal) { nd.taken = CrNode::Internal; nd.target = index(target); }
                    else if (tk.kind != Target::Outside) { nd.taken = CrNode::Tail; nd.tail = resolve_callee(p, f, target); }
                    nd.fall = bc && !((bo & 4) && (bo & 16));
                }
                break;
            }
            case Kind::BCLR: {
                const uint32_t bo = ppc::bo(w), bi = ppc::bi(w);
                if (!(bo & 16)) nd.use |= fld(bi >> 2);
                if (ppc::lk(w)) nd.call = {Callee::Indirect, 0, 0};
                else { nd.taken = CrNode::Return; nd.fall = !((bo & 4) && (bo & 16)); }
                break;
            }
            case Kind::BCCTR: {
                const uint32_t bo = ppc::bo(w), bi = ppc::bi(w);
                const bool table = !ppc::lk(w) && std::any_of(p.an.jump_tables.begin(), p.an.jump_tables.end(), [&](const auto& jt) { return jt.bctr_addr == pc; });
                if (table) {
                    nd.fall = false;   // the switch ignores the branch condition
                    for (const auto& jt : p.an.jump_tables)
                        if (jt.bctr_addr == pc) for (uint32_t t : jt.targets) if (t >= f.entry && t < f.end) nd.table.push_back(index(t));
                    break;
                }
                if (!(bo & 16)) nd.use |= fld(bi >> 2);
                if (ppc::lk(w)) nd.call = {Callee::Indirect, 0, 0};
                else { nd.taken = CrNode::IndirectTail; nd.fall = !(bo & 16); }
                break;
            }
            default: break;
        }
    }
    return nodes;
}

struct CrFn {
    const Function* f = nullptr;
    std::vector<CrNode> nodes;
    std::vector<uint8_t> in, out;
    Callee fall_tail;   // the code after the last instruction continues into the next function
};
}  // namespace

CrLive analyze_cr(const Program& p) {
    std::vector<CrFn> fns;
    for (const auto& f : p.an.functions) {
        if (p.cfg.hle.count(f.entry) || f.end <= f.entry) continue;
        CrFn fn;
        fn.f = &f;
        fn.nodes = cr_nodes(p, f);
        fn.in.assign(fn.nodes.size(), 0); fn.out.assign(fn.nodes.size(), 0);
        uint32_t last = 0;
        p.img.word(f.end - 4, last);
        if (!is_terminator_word(last) && p.an.is_entry(f.end)) fn.fall_tail = resolve_callee(p, f, f.end);
        fns.push_back(std::move(fn));
    }
    std::unordered_map<uint32_t, uint8_t> entry_live, ret_live;   // entry/secondary address -> fields read on entry; function -> fields read after it returns
    uint8_t any_entry = 0, any_ret = 0;                           // unknown (indirect) callees / callers
    auto grow = [](uint8_t& dst, uint8_t add) { const uint8_t before = dst; dst |= add; return dst != before; };
    auto call_in = [&](const Callee& c) -> uint8_t {
        if (c.kind == Callee::Direct) { auto it = entry_live.find(c.entry); return it == entry_live.end() ? 0 : it->second; }
        return c.kind == Callee::Indirect ? any_entry : 0;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& fn : fns) {
            const Function& f = *fn.f;
            const size_t n = fn.nodes.size();
            const uint8_t ret_f = uint8_t(ret_live[f.entry] | any_ret);
            const uint8_t end_in = fn.fall_tail.kind == Callee::None ? 0 : uint8_t(call_in(fn.fall_tail) | ret_f);
            for (bool again = true; again;) {
                again = false;
                for (size_t i = n; i-- > 0;) {
                    const CrNode& nd = fn.nodes[i];
                    uint8_t o = 0;
                    if (nd.fall) o |= i + 1 < n ? fn.in[i + 1] : end_in;
                    switch (nd.taken) {
                        case CrNode::Internal: o |= fn.in[size_t(nd.target)]; break;
                        case CrNode::Return: o |= ret_f; break;
                        case CrNode::Tail: o |= uint8_t(call_in(nd.tail) | ret_f); break;
                        case CrNode::IndirectTail: o |= uint8_t(any_entry | ret_f); break;
                        default: break;
                    }
                    for (int t : nd.table) o |= fn.in[size_t(t)];
                    const uint8_t ni = uint8_t(nd.use | (o & ~nd.def) | call_in(nd.call));
                    fn.out[i] = o;
                    if (ni != fn.in[i]) { fn.in[i] = ni; again = true; }
                }
            }
            // publish this function's summary
            if (n) {
                changed |= grow(entry_live[f.entry], fn.in[0]);
                changed |= grow(any_entry, fn.in[0]);
                auto sec = p.an.secondary.find(f.entry);
                if (sec != p.an.secondary.end())
                    for (uint32_t t : sec->second) if (t >= f.entry && t < f.end) { changed |= grow(entry_live[t], fn.in[size_t((t - f.entry) / 4)]); changed |= grow(any_entry, fn.in[size_t((t - f.entry) / 4)]); }
            }
            for (size_t i = 0; i < n; ++i) {
                const CrNode& nd = fn.nodes[i];
                if (nd.call.kind == Callee::Direct) changed |= grow(ret_live[nd.call.fn], fn.out[i]);
                else if (nd.call.kind == Callee::Indirect) changed |= grow(any_ret, fn.out[i]);
                if (nd.taken == CrNode::Tail && nd.tail.kind == Callee::Direct) changed |= grow(ret_live[nd.tail.fn], ret_f);
                else if (nd.taken == CrNode::IndirectTail) changed |= grow(any_ret, ret_f);
            }
            if (fn.fall_tail.kind == Callee::Direct) changed |= grow(ret_live[fn.fall_tail.fn], ret_f);
        }
    }
    CrLive live;
    for (auto& fn : fns) live.out[fn.f->entry] = std::move(fn.out);
    return live;
}

std::string emit_function(const Program& p, const Function& f, EmitStats& st, const CrLive* live) {
    std::set<uint32_t> labels{f.entry};
    for (uint32_t pc = f.entry; pc < f.end; pc += 4) {
        uint32_t w = 0;
        if (!p.img.word(pc, w)) continue;
        const ppc::OpInfo* op = ppc::decode(w);
        if (!op) continue;
        if (op->kind == ppc::Kind::BR || op->kind == ppc::Kind::BC) {
            if (ppc::lk(w)) continue;
            int32_t d = op->kind == ppc::Kind::BR ? ppc::li(w) : ppc::bd(w);
            uint32_t t = ppc::aa(w) ? uint32_t(d) : pc + uint32_t(d);
            if (t >= f.entry && t < f.end) labels.insert(t);
        }
    }
    for (const auto& jt : p.an.jump_tables) if (jt.bctr_addr >= f.entry && jt.bctr_addr < f.end) for (uint32_t t : jt.targets) labels.insert(t);
    auto sit = p.an.secondary.find(f.entry);
    bool has_sec = sit != p.an.secondary.end();
    if (has_sec) for (uint32_t t : sit->second) labels.insert(t);

    std::ostringstream o;
    if (has_sec) {
        o << "static void body_" << hex8(f.entry) << "(ppc::Ctx& c, uint32_t at) {\n  UC_ENTER(0x" << hex8(f.entry) << "u);\n  /*CFDECL*/\n  switch (at) { ";
        for (uint32_t t : sit->second) o << "case 0x" << hex8(t) << "u: goto L_" << hex8(t) << "; ";
        o << "default: break; }\n";
    } else {
        o << "void " << fn_name(f.entry) << "(ppc::Ctx& c) {\n  UC_ENTER(0x" << hex8(f.entry) << "u);\n  /*CFDECL*/\n";
    }
    const std::vector<uint8_t>* after = nullptr;   // CR fields still readable after each instruction
    if (live) { auto it = live->out.find(f.entry); if (it != live->out.end()) after = &it->second; }
    FusedCmp fused;
    std::map<uint32_t, FusedCmp> pending_fuse;   // branch address -> the earlier compare (operands kept in cf<field>a/b) it tests
    uint8_t cf_used = 0;                         // CR fields that have cf<field>a/b locals
    // The branch (at most 8 instructions on, none of them a label, another branch, a call, or a reader or writer of the field) that is the last reader of a compare's CR field.
    const auto find_reader = [&](uint32_t from, uint32_t field) -> uint32_t {
        uint32_t at = from + 4;
        for (unsigned n = 0; n < 8 && at < f.end && !labels.count(at); at += 4, ++n) {
            uint32_t word = 0;
            if (!p.img.word(at, word)) return 0;
            const ppc::OpInfo* o2 = ppc::decode(word);
            if (!o2 || o2->kind == ppc::Kind::UNSUP) return 0;
            if (o2->kind == ppc::Kind::BC || o2->kind == ppc::Kind::BCLR)
                return !ppc::lk(word) && (ppc::bo(word) & 4) && !(ppc::bo(word) & 16) && (ppc::bi(word) >> 2) == field && !((*after)[(at - f.entry) / 4] & fld(field)) ? at : 0;
            if (o2->kind == ppc::Kind::BR || o2->kind == ppc::Kind::BCCTR || o2->kind == ppc::Kind::SC) return 0;
            const CrEffect fx = cr_effect(*o2, word);
            if ((fx.use | fx.def) & fld(field)) return 0;
        }
        return 0;
    };
    bool fuse_next = false;   // the previous compare was folded into this instruction's branch
    std::deque<std::pair<std::string, bool>> queued;   // code (and whether the branch after it tests `fused`) for the next instructions, which an earlier fcmp already folded
    for (uint32_t pc = f.entry; pc < f.end; pc += 4) {
        uint32_t w = 0;
        p.img.word(pc, w);
        if (labels.count(pc)) o << "L_" << hex8(pc) << ":;\n";
        CrHints hints;
        if (fuse_next) { hints.fused = &fused; fuse_next = false; }
        else if (const auto pf = pending_fuse.find(pc); pf != pending_fuse.end()) hints.fused = &pf->second;
        std::string code;
        const ppc::OpInfo* op = after ? ppc::decode(w) : nullptr;
        if (!queued.empty()) {   // an instruction of a folded fcmp sequence
            code = std::move(queued.front().first); fuse_next = queued.front().second; queued.pop_front();
        } else if (op && op->kind != ppc::Kind::UNSUP && !(op->sub == ppc::Sub::X9 && ppc::oe(w)) && !((std::string(op->name) == "twi" || std::string(op->name) == "tw") && ppc::t(w) == 31)) {
            const uint8_t live_after = (*after)[(pc - f.entry) / 4];
            const std::string name = op->name;
            if (name == "cmp" || name == "cmpl" || name == "cmpi" || name == "cmpli") {
                const uint32_t field = ppc::crfd(w);
                uint32_t next = 0;
                const ppc::OpInfo* nop = pc + 4 < f.end && !labels.count(pc + 4) && p.img.word(pc + 4, next) ? ppc::decode(next) : nullptr;
                if (!(live_after & fld(field))) { code = "/* cr" + num(field) + " is never read */"; ++st.cr_dead_compares; }
                else if (nop && (nop->kind == ppc::Kind::BC || nop->kind == ppc::Kind::BCLR) && !ppc::lk(next) && (ppc::bo(next) & 4) && !(ppc::bo(next) & 16) &&
                         (ppc::bi(next) >> 2) == field && !((*after)[(pc + 4 - f.entry) / 4] & fld(field))) {
                    // the only reader is the next branch: test the operands there and skip the CR write
                    const std::string a = "c.r[" + num(ppc::a(w)) + "]";
                    const bool is_signed = name == "cmp" || name == "cmpi";
                    const std::string lhs = is_signed ? "int32_t(" + a + ")" : a;
                    std::string rhs;
                    if (name == "cmp" || name == "cmpl") rhs = is_signed ? "int32_t(c.r[" + num(ppc::b(w)) + "])" : "c.r[" + num(ppc::b(w)) + "]";
                    else rhs = is_signed ? num(ppc::simm(w)) : num(ppc::uimm(w)) + "u";
                    fused = {lhs, rhs};
                    fuse_next = true;
                    code = "/* cr" + num(field) + " compare is folded into the next branch */";
                    ++st.cr_fused_branches;
                } else if (const uint32_t reader = find_reader(pc, field)) {
                    // the compare feeds a branch a few instructions later (the scheduler moved work in between): keep the operands in locals
                    const bool is_signed = name == "cmp" || name == "cmpi";
                    const std::string var = "cf" + num(field), a = var + "a", b = var + "b";
                    std::string rhs;
                    if (name == "cmp" || name == "cmpl") rhs = "c.r[" + num(ppc::b(w)) + "]";
                    else rhs = is_signed ? "uint32_t(" + num(ppc::simm(w)) + ")" : num(ppc::uimm(w)) + "u";
                    cf_used |= fld(field);
                    pending_fuse[reader] = is_signed ? FusedCmp{"int32_t(" + a + ")", "int32_t(" + b + ")"} : FusedCmp{a, b};
                    code = a + " = c.r[" + num(ppc::a(w)) + "]; " + b + " = " + rhs + ";";
                    ++st.cr_fused_branches;
                }
            } else if (name == "fcmpu" || name == "fcmpo") {
                const uint32_t field = ppc::crfd(w);
                uint32_t n1 = 0, n2 = 0;
                const auto next_op = [&](uint32_t at, uint32_t& word) { return at < f.end && !labels.count(at) && p.img.word(at, word) ? ppc::decode(word) : nullptr; };
                // a plain conditional branch on this field that is its last reader
                const auto last_reader = [&](const ppc::OpInfo* b, uint32_t word, uint32_t at) {
                    return b && (b->kind == ppc::Kind::BC || b->kind == ppc::Kind::BCLR) && !ppc::lk(word) && (ppc::bo(word) & 4) && !(ppc::bo(word) & 16) &&
                           (ppc::bi(word) >> 2) == field && !((*after)[(at - f.entry) / 4] & fld(field));
                };
                const std::string lhs = "c.f[" + num(ppc::a(w)) + "]", rhs = "c.f[" + num(ppc::b(w)) + "]";
                const ppc::OpInfo* o1 = next_op(pc + 4, n1);
                const bool is_cror = o1 && std::string(o1->name) == "cror" && ppc::t(n1) == 4 * field + 2 && ppc::b(n1) == 4 * field + 2 && (ppc::a(n1) == 4 * field || ppc::a(n1) == 4 * field + 1);
                const char* const cror_op = is_cror ? (ppc::a(n1) == 4 * field ? "<=" : ">=") : nullptr;
                // [cror] mfcr rT; rlwinm[.] rT,rT,sh,31,31: the GPR gets one bit of the compare (the whole CR word is dead, rT is overwritten)
                const uint32_t mfcr_at = pc + (is_cror ? 8 : 4);
                uint32_t n3 = 0, n4 = 0;
                const ppc::OpInfo* o3 = next_op(mfcr_at, n3);
                const ppc::OpInfo* o4 = o3 && std::string(o3->name) == "mfcr" ? next_op(mfcr_at + 4, n4) : nullptr;
                const uint32_t bit = (ppc::sh(n4) + 31) & 31;
                const char* bit_op = nullptr;   // the comparison that bit holds
                if (o4 && std::string(o4->name) == "rlwinm" && ppc::t(n3) == ppc::t(n4) && ppc::a(n4) == ppc::t(n3) && ppc::mb(n4) == 31 && ppc::me(n4) == 31 && bit / 4 == field) {
                    if (cror_op) { if (bit == 4 * field + 2) bit_op = cror_op; }
                    else if (bit % 4 == 0) bit_op = "<"; else if (bit % 4 == 1) bit_op = ">"; else if (bit % 4 == 2) bit_op = "==";
                }
                const bool bit_rc = bit_op && ppc::rc(n4);
                const uint32_t last_at = mfcr_at + 4;   // the rlwinm
                const bool field_dead = bit_op && ((bit_rc && field == 0) || !((*after)[(last_at - f.entry) / 4] & fld(field)));   // rlwinm. rewrites cr0 as a whole
                if (!(live_after & fld(field))) { code = "/* cr" + num(field) + " is never read */"; ++st.cr_dead_compares; }
                else if (bit_op && field_dead) {
                    const std::string reg = "c.r[" + num(ppc::t(n3)) + "]";
                    uint32_t nb = 0;
                    const ppc::OpInfo* ob = bit_rc ? next_op(last_at + 4, nb) : nullptr;
                    const bool cr0_live = (*after)[(last_at - f.entry) / 4] & fld(0);
                    // the record form's only reader is the next branch: it tests the register directly
                    const bool cr0_branch = ob && (ob->kind == ppc::Kind::BC || ob->kind == ppc::Kind::BCLR) && !ppc::lk(nb) && (ppc::bo(nb) & 4) && !(ppc::bo(nb) & 16) &&
                                            (ppc::bi(nb) >> 2) == 0 && !((*after)[(last_at + 4 - f.entry) / 4] & fld(0));
                    std::string tail = reg + " = uint32_t(" + lhs + " " + bit_op + " " + rhs + ");";
                    if (bit_rc && !cr0_branch && cr0_live) tail += " ppc::cr0(c, " + reg + ");";
                    if (cr0_branch) fused = {"int32_t(" + reg + ")", "0"};
                    code = "/* cr" + num(field) + " compare is folded into the bit test below */";
                    if (is_cror) queued.push_back({"/* cror is folded into the bit test below */", false});
                    queued.push_back({"/* mfcr is folded into the bit test below */", false});
                    queued.push_back({tail, cr0_branch});
                    ++st.cr_fused_branches;
                } else if (last_reader(o1, n1, pc + 4)) {
                    fused = {lhs, rhs, true, nullptr};
                    fuse_next = true;
                    code = "/* cr" + num(field) + " compare is folded into the next branch */";
                    ++st.cr_fused_branches;
                } else if (is_cror) {
                    // fcmp, cror EQ = LT|EQ (or GT|EQ), branch on EQ: one <= (or >=) test; the CR field is dead after the branch
                    const ppc::OpInfo* o2 = next_op(pc + 8, n2);
                    if (last_reader(o2, n2, pc + 8) && (ppc::bi(n2) & 3) == 2) {
                        fused = {lhs, rhs, true, cror_op};
                        queued.push_back({"/* cror is folded into the next branch */", true});
                        code = "/* cr" + num(field) + " compare, cror and branch are folded into one test */";
                        ++st.cr_fused_branches;
                    }
                }
            } else if (op->rc && ppc::rc(w) && !fp_name(name) && (live_after & fld(0)) && rc_dest(name, w) >= 0 &&
                       [&] {   // the record form's only reader is the next branch: it tests the result register directly
                           uint32_t nb = 0;
                           const ppc::OpInfo* ob = pc + 4 < f.end && !labels.count(pc + 4) && p.img.word(pc + 4, nb) ? ppc::decode(nb) : nullptr;
                           return ob && (ob->kind == ppc::Kind::BC || ob->kind == ppc::Kind::BCLR) && !ppc::lk(nb) && (ppc::bo(nb) & 4) && !(ppc::bo(nb) & 16) &&
                                  (ppc::bi(nb) >> 2) == 0 && !((*after)[(pc + 4 - f.entry) / 4] & fld(0));
                       }()) {
                hints.drop_rc = true;
                fused = {"int32_t(c.r[" + num(rc_dest(name, w)) + "])", "0"};
                fuse_next = true;
                ++st.cr_dropped_records; ++st.cr_fused_branches;
            } else if (op->rc && ppc::rc(w) && !(live_after & fld(fp_name(name) ? 1 : 0))) {
                hints.drop_rc = true;
                ++st.cr_dropped_records;
            }
        }
        if (code.empty()) code = emit_insn(p, f, pc, w, st, &hints);
        else ++st.instructions;
        o << "  /*" << hex8(pc) << "*/ { " << code << " }\n";
    }
    uint32_t last = 0;
    p.img.word(f.end - 4, last);
    if (!is_terminator_word(last) && p.an.is_entry(f.end)) {
        ++st.fallthrough_tail;
        o << "  " << callee(p, f.end) << "(c); return;\n}\n";
    } else {
        o << "  " << trap_str(f.end, "fell off the end of function " + hex8(f.entry)) << "\n}\n";
    }
    if (has_sec) {
        o << "void " << fn_name(f.entry) << "(ppc::Ctx& c) { body_" << hex8(f.entry) << "(c, 0); }\n";
        for (uint32_t t : sit->second) {
            o << "void " << sec_name(f.entry, t) << "(ppc::Ctx& c) { body_" << hex8(f.entry) << "(c, 0x" << hex8(t) << "u); }\n";
            ++st.secondary_entries;
        }
    }
    ++st.functions;
    std::string text = o.str();
    std::string decl;
    for (uint32_t field = 0; field < 8; ++field)
        if (cf_used & fld(field)) decl += "uint32_t cf" + num(field) + "a, cf" + num(field) + "b;";   // compare operands waiting for a later branch
    const size_t marker = text.find("  /*CFDECL*/\n");
    text.replace(marker, 13, decl.empty() ? "" : "  " + decl + "\n");
    return text;
}

Output emit_program(const Program& p, size_t shard, EmitStats& st) {
    Output out;
    std::set<std::string> hle_names;
    for (auto& [a, n] : p.cfg.hle) hle_names.insert(n);

    std::string decl = "#pragma once\n#include \"ppc_rt.h\"\n#include \"dispatch.h\"\n";
    for (const auto& f : p.an.functions) {
        if (p.cfg.hle.count(f.entry)) continue;
        decl += "void " + fn_name(f.entry) + "(ppc::Ctx&);\n";
        auto it = p.an.secondary.find(f.entry);
        if (it != p.an.secondary.end()) for (uint32_t t : it->second) decl += "void " + sec_name(f.entry, t) + "(ppc::Ctx&);\n";
    }
    for (const auto& n : hle_names) decl += "void hle_" + n + "(ppc::Ctx&);\n";
    out.files.push_back({"game_decl.h", decl});

    size_t idx = 0, in_cur = 0;
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        char nm[32]; std::snprintf(nm, sizeof nm, "game_%03zu.cpp", idx++);
        out.files.push_back({nm, "#include \"game_decl.h\"\n" + cur});
        cur.clear(); in_cur = 0;
    };
    const CrLive live = p.cr_opt ? analyze_cr(p) : CrLive{};
    for (const auto& f : p.an.functions) {
        if (p.cfg.hle.count(f.entry)) continue;
        cur += emit_function(p, f, st, p.cr_opt ? &live : nullptr);
        if (++in_cur >= shard) flush();
    }
    flush();

    std::string table = "#include \"game_decl.h\"\nextern const uc::FnEntry uc_functions[] = {\n";
    size_t table_count = p.an.functions.size();
    for (const auto& f : p.an.functions) {
        table += "  {0x" + hex8(f.entry) + "u, " + callee(p, f.entry) + "},\n";
        auto it = p.an.secondary.find(f.entry);
        if (it != p.an.secondary.end() && !p.cfg.hle.count(f.entry)) {
            for (uint32_t t : it->second) { table += "  {0x" + hex8(t) + "u, " + sec_name(f.entry, t) + "},\n"; ++table_count; }
        }
    }
    table += "};\nextern const size_t uc_function_count = " + std::to_string(table_count) + ";\n";
    uint32_t tb = 0xFFFFFFFFu, te = 0;
    for (const auto& s : p.img.dol->sections) if (s.is_text) { tb = std::min(tb, s.address); te = std::max(te, s.address + s.size); }
    table += "extern const uint32_t uc_text_begin = 0x" + hex8(tb) + "u;\nextern const uint32_t uc_text_end = 0x" + hex8(te) + "u;\n";
    out.files.push_back({"game_table.cpp", table});
    return out;
}

}  // namespace uc
