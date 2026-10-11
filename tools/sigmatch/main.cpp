#include "analyzer/analyzer.h"
#include "ppc/insn.h"
#include "sig/dsy.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>

// Database names look like "OSCreateThread 	os.a OSThread.o": the function name is the first token, the rest names library and object.
static std::string base_name(const std::string& n) { return n.substr(0, n.find_first_of(" 	")); }

static bool is_term(uint32_t w) { return w == 0x4E800020u || w == 0x4E800420u || ((w >> 26) == 18 && !(w & 1)); }

static std::string c_string(const uc::Image& img, uint32_t addr) {
    const uc::DolSection* s = img.section(addr);
    if (!s) return {};
    std::string r;
    for (uint32_t a = addr; a < s->address + s->size && r.size() < 160; ++a) {
        char ch = char(img.file[s->file_offset + (a - s->address)]);
        if (!ch) break;
        if (ch < 0x20 || ch > 0x7E) return {};
        r.push_back(ch);
    }
    return r.size() >= 4 ? r : std::string{};
}

int main(int argc, char** argv) {
    std::string dol_path, dsy_path, cfg_path, out_dir, required_path;
    size_t min_words = 6;
    const char* usage = "usage: sigmatch --dol <f> [--dsy <f>] [--cfg <f>] [--required <f>] [--min-words N] --out <dir>\n";
    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* n) { return !std::strcmp(argv[i], n) && i + 1 < argc; };
        if (arg("--dol")) dol_path = argv[++i];
        else if (arg("--dsy")) dsy_path = argv[++i];
        else if (arg("--cfg")) cfg_path = argv[++i];
        else if (arg("--out")) out_dir = argv[++i];
        else if (arg("--required")) required_path = argv[++i];
        else if (arg("--min-words")) min_words = std::strtoull(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "%s", usage); return 2; }
    }
    if (dol_path.empty() || out_dir.empty()) { std::fprintf(stderr, "%s", usage); return 2; }

    std::ifstream in(dol_path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", dol_path.c_str()); return 1; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto dol = uc::parse_dol(file);
    if (!dol.ok) { std::fprintf(stderr, "invalid DOL: %s\n", dol.error.c_str()); return 1; }
    uc::Config cfg; std::string err;
    if (!cfg_path.empty() && !uc::load_config(cfg_path, cfg, err)) { std::fprintf(stderr, "config error: %s\n", err.c_str()); return 1; }
    uc::Image img{file, &dol.image};
    uc::Analysis an = uc::analyze(img, cfg);

    std::filesystem::create_directories(out_dir);
    std::map<uint32_t, std::string> names;     // entry -> name
    std::set<uint32_t> ambiguous;

    if (!dsy_path.empty()) {
        std::vector<uc::DsyEntry> db;
        if (!uc::load_dsy(dsy_path, db, err)) { std::fprintf(stderr, "dsy error: %s\n", err.c_str()); return 1; }
        std::multimap<uint32_t, const uc::DsyEntry*> by_sum;
        for (auto& e : db) by_sum.emplace(e.checksum, &e);
        for (const auto& f : an.functions) {
            uint32_t sum = 0;
            for (uint32_t pc = f.entry; pc < f.end; pc += 4) {
                uint32_t w = 0; img.word(pc, w);
                sum = uc::dsy_step(sum, w);
                size_t words = (pc - f.entry) / 4 + 1;
                if (!is_term(w) || words < min_words) continue;
                auto [lo, hi] = by_sum.equal_range(sum);
                std::set<std::string> cand;
                for (auto it = lo; it != hi; ++it) if (it->second->size == words * 4) cand.insert(it->second->name);
                if (cand.empty()) continue;
                std::set<std::string> bases;
                for (auto& c : cand) bases.insert(base_name(c));
                if (bases.size() == 1) names[f.entry] = *cand.begin();
                else { ambiguous.insert(f.entry); std::string j; for (auto& c : cand) j += (j.empty() ? "" : "|") + c; names[f.entry] = "AMBIGUOUS " + j; }
            }
        }
    }

    // string anchors: lis rX,hi ; addi rY,rX,lo -> address of a string that looks like it names an SDK API
    std::regex sdk(R"((OS|DVD|WPAD|KPAD|VI|GX|AX|NAND|PAD|SI|EXI|CARD|MEM)[A-Za-z]*(\(\)|\.c)|Panic|RVL_SDK|[A-Za-z]+\.c$)");
    std::ofstream af(std::filesystem::path(out_dir) / "anchors.txt");
    size_t anchors = 0;
    char buf[16];
    for (const auto& f : an.functions) {
        for (uint32_t pc = f.entry; pc + 4 < f.end; pc += 4) {
            uint32_t w = 0; img.word(pc, w);
            if ((w >> 26) != 15 || ppc::a(w) != 0) continue;   // lis rX,hi
            for (uint32_t d = 1; d <= 8 && pc + 4 * d < f.end; ++d) {   // addi rY,rX,lo within a few instructions
                uint32_t w2 = 0; img.word(pc + 4 * d, w2);
                if ((w2 >> 26) != 14 || ppc::a(w2) != ppc::t(w)) continue;
                uint32_t addr = (w & 0xFFFF) << 16; addr += uint32_t(ppc::simm(w2));
                std::string s = c_string(img, addr);
                if (s.empty() || !std::regex_search(s, sdk)) continue;
                std::snprintf(buf, sizeof buf, "%08X", f.entry);
                af << buf << " \"" << s << "\"\n";
                ++anchors;
                break;
            }
        }
    }

    std::ofstream nf(std::filesystem::path(out_dir) / "names.txt");
    for (auto& [a, n] : names) { std::snprintf(buf, sizeof buf, "%08X", a); nf << buf << ' ' << n << '\n'; }

    std::map<std::string, size_t> prefix;
    for (auto& [a, n] : names) {
        if (n.rfind("AMBIGUOUS", 0) == 0) continue;
        std::string b = base_name(n);
        size_t i = 0; while (i < b.size() && (b[i] == '_' || std::isupper(static_cast<unsigned char>(b[i])))) ++i;
        ++prefix[b.substr(0, std::min<size_t>(i, 6))];
    }
    std::printf("functions %zu  named %zu  ambiguous %zu  string-anchors %zu\n", an.functions.size(), names.size() - ambiguous.size(), ambiguous.size(), anchors);
    for (auto& [p, c] : prefix) std::printf("  %-8s %zu\n", p.c_str(), c);

    if (!required_path.empty()) {
        std::set<std::string> found_names;
        for (auto& [a, n] : names) if (n.rfind("AMBIGUOUS", 0) != 0) found_names.insert(base_name(n));
        // names promoted into uc.cfg as `hle <addr> <Name>` count as identified
        for (auto& [a, n] : cfg.hle) found_names.insert(n);
        std::ifstream rq(required_path);
        std::map<std::string, std::pair<int, int>> cat;   // category -> found/total
        std::string line;
        while (std::getline(rq, line)) {
            std::istringstream ls(line);
            std::string c, n;
            if (!(ls >> c >> n)) continue;
            bool ok = false;   // `A|B` lists alternative names (public API versus the internal symbol in the binary)
            for (size_t from = 0; from <= n.size();) {
                size_t bar = n.find('|', from);
                if (found_names.count(n.substr(from, bar == std::string::npos ? std::string::npos : bar - from))) ok = true;
                if (bar == std::string::npos) break;
                from = bar + 1;
            }
            ++cat[c].second; if (ok) ++cat[c].first;
            std::printf("%s %s %s\n", ok ? "FOUND  " : "MISSING", c.c_str(), n.c_str());
        }
        for (auto& [c, fr] : cat) std::printf("%s: %d/%d\n", c.c_str(), fr.first, fr.second);
    }
    return 0;
}
