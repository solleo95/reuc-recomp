#include "analyzer/analyzer.h"
#include "ppc/format.h"
#include "ppc/insn.h"
#include "ppc/isa.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

static std::map<uint32_t, std::string> g_names;

static void load_names(const std::string& path) {   // "<hex> <name> ..." lines; '#' comments
    std::ifstream f(path);
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string a, n;
        if (!(ls >> a >> n)) continue;
        g_names[uint32_t(std::stoul(a, nullptr, 16))] = n;
    }
}
static std::string name_of(uint32_t a) { auto it = g_names.find(a); return it == g_names.end() ? "" : it->second; }

static bool resolve(const std::string& s, uint32_t& out) {
    for (auto& [a, n] : g_names) if (n == s) { out = a; return true; }
    char* end = nullptr;
    unsigned long v = std::strtoul(s.c_str(), &end, 16);
    if (end && !*end && !s.empty()) { out = uint32_t(v); return true; }
    return false;
}

int main(int argc, char** argv) {
    std::string dol_path, cfg_path, cmd, arg;
    int count = 24;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--dol") dol_path = next();
        else if (a == "--cfg") cfg_path = next();
        else if (a == "--names") load_names(next());
        else if (a == "--disasm" || a == "--callers" || a == "--callees" || a == "--find-string") {
            cmd = a; arg = next();
            if (cmd == "--disasm" && i + 1 < argc && argv[i + 1][0] != '-') count = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: xref --dol <f> [--cfg <f>] [--names <f>]... (--disasm <addr> [n] | --callers <x> | --callees <x> | --find-string <text>)\n");
            return 2;
        }
    }
    if (dol_path.empty() || cmd.empty()) { std::fprintf(stderr, "missing --dol or command\n"); return 2; }
    std::ifstream in(dol_path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", dol_path.c_str()); return 1; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto dol = uc::parse_dol(file);
    if (!dol.ok) { std::fprintf(stderr, "invalid DOL: %s\n", dol.error.c_str()); return 1; }
    uc::Config cfg; std::string err;
    if (!cfg_path.empty() && !uc::load_config(cfg_path, cfg, err)) { std::fprintf(stderr, "config error: %s\n", err.c_str()); return 1; }
    uc::Image img{file, &dol.image};
    uc::Analysis an = uc::analyze(img, cfg);

    auto label = [&](uint32_t a) {
        char b[24]; std::snprintf(b, sizeof b, "%08X", a);
        std::string n = name_of(a);
        if (n.empty()) if (auto* f = an.find_containing(a)) n = name_of(f->entry) + (f->entry == a ? "" : "+" + std::to_string(a - f->entry));
        return std::string(b) + (n.empty() || n[0] == '+' ? "" : " <" + n + ">");
    };

    if (cmd == "--disasm") {
        uint32_t a = 0;
        if (!resolve(arg, a)) { std::fprintf(stderr, "unknown address or name '%s'\n", arg.c_str()); return 1; }
        for (int k = 0; k < count; ++k, a += 4) {
            uint32_t w = 0;
            if (!img.word(a, w)) break;
            std::printf("%s: %08X  %s\n", label(a).c_str(), w, ppc::format_insn(a, w).c_str());
        }
        return 0;
    }
    if (cmd == "--find-string") {   // functions that build the address of a string containing `arg` (lis + addi within 8 insns)
        std::set<uint32_t> hits;
        for (const auto& s : dol.image.sections) {
            std::string text(reinterpret_cast<const char*>(file.data()) + s.file_offset, s.size);
            for (size_t p = text.find(arg); p != std::string::npos; p = text.find(arg, p + 1)) {
                size_t st = p; while (st > 0 && text[st - 1] >= 0x20 && text[st - 1] < 0x7F) --st;
                hits.insert(s.address + uint32_t(st));
            }
        }
        for (uint32_t str : hits) {
            std::printf("string at %08X\n", str);
            for (const auto& f : an.functions)
                for (uint32_t pc = f.entry; pc + 4 < f.end; pc += 4) {
                    uint32_t w = 0; img.word(pc, w);
                    if ((w >> 26) != 15 || ppc::a(w) != 0) continue;
                    for (uint32_t d = 1; d <= 8 && pc + 4 * d < f.end; ++d) {
                        uint32_t w2 = 0; img.word(pc + 4 * d, w2);
                        if ((w2 >> 26) != 14 || ppc::a(w2) != ppc::t(w)) continue;
                        if (((w & 0xFFFF) << 16) + uint32_t(ppc::simm(w2)) == str) { std::printf("  used in %s at %08X\n", label(f.entry).c_str(), pc); break; }
                    }
                }
        }
        return 0;
    }
    uint32_t target = 0;
    if (!resolve(arg, target)) { std::fprintf(stderr, "unknown address or name '%s'\n", arg.c_str()); return 1; }
    const uc::Function* tf = an.find_containing(target);
    if (cmd == "--callees") {
        if (!tf) { std::fprintf(stderr, "no function at %08X\n", target); return 1; }
        std::set<uint32_t> seen;
        for (uint32_t pc = tf->entry; pc < tf->end; pc += 4) {
            uint32_t w = 0; img.word(pc, w);
            if ((w >> 26) == 18 && (w & 3) == 1) { uint32_t t = pc + uint32_t(ppc::li(w)); if (seen.insert(t).second) std::printf("%s\n", label(t).c_str()); }
        }
        return 0;
    }
    for (const auto& f : an.functions) {   // --callers: bl/b to `target`, plus data words holding its address
        for (uint32_t pc = f.entry; pc < f.end; pc += 4) {
            uint32_t w = 0; img.word(pc, w);
            if ((w >> 26) == 18 && !(w & 2) && pc + uint32_t(ppc::li(w)) == target) std::printf("%s  at %08X (%s)\n", label(f.entry).c_str(), pc, (w & 1) ? "bl" : "b");
        }
    }
    for (const auto& s : dol.image.sections) {
        if (s.is_text) continue;
        for (uint32_t a = s.address; a + 4 <= s.address + s.size; a += 4) { uint32_t v = 0; img.word(a, v); if (v == target) std::printf("data pointer at %08X\n", a); }
    }
    return 0;
}
