#include "translator/emit.h"
#include "ppc/insn.h"
#include "ppc/isa.h"
#include <algorithm>
#include <cstdio>
#include <set>
#include <sstream>

namespace uc {

static std::string hex8(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08X", v); return b; }
static std::string num(long long v) { return std::to_string(v); }
static std::string fn_name(uint32_t a) { return "fn_" + hex8(a); }
static std::string sec_name(uint32_t owner, uint32_t t) { return "fn_" + hex8(owner) + "_" + hex8(t); }

static std::string callee(const Program& p, uint32_t target) {
    auto it = p.cfg.hle.find(target);
    return it != p.cfg.hle.end() ? "hle_" + it->second : fn_name(target);
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

std::string emit_insn(const Program& p, const Function& f, uint32_t pc, uint32_t w, EmitStats& st) {
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
            std::string cond = cond_expr(bo, bi, pre);
            return "{ " + pre + "if (" + cond + ") { " + branch_action(p, f, pc, target, ppc::lk(w), st) + " } }";
        }
        case Kind::BCLR: {
            uint32_t bo = ppc::bo(w), bi = ppc::bi(w);
            std::string pre;
            std::string cond = cond_expr(bo, bi, pre);
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
    if (op->rc) a += (a.empty() ? "" : ", ") + std::string(ppc::rc(w) ? "true" : "false");
    return "ppc::op_" + name + "(c" + (a.empty() ? "" : ", " + a) + ");";
}

static bool is_terminator_word(uint32_t w) { return w == 0x4E800020u || w == 0x4E800420u || ((w >> 26) == 18 && !(w & 1)); }

std::string emit_function(const Program& p, const Function& f, EmitStats& st) {
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
        o << "static void body_" << hex8(f.entry) << "(ppc::Ctx& c, uint32_t at) {\n  UC_ENTER(0x" << hex8(f.entry) << "u);\n  switch (at) { ";
        for (uint32_t t : sit->second) o << "case 0x" << hex8(t) << "u: goto L_" << hex8(t) << "; ";
        o << "default: break; }\n";
    } else {
        o << "void " << fn_name(f.entry) << "(ppc::Ctx& c) {\n  UC_ENTER(0x" << hex8(f.entry) << "u);\n";
    }
    for (uint32_t pc = f.entry; pc < f.end; pc += 4) {
        uint32_t w = 0;
        p.img.word(pc, w);
        if (labels.count(pc)) o << "L_" << hex8(pc) << ":;\n";
        o << "  /*" << hex8(pc) << "*/ { " << emit_insn(p, f, pc, w, st) << " }\n";
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
    return o.str();
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
    for (const auto& f : p.an.functions) {
        if (p.cfg.hle.count(f.entry)) continue;
        cur += emit_function(p, f, st);
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
