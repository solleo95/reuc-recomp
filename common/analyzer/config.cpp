#include "analyzer/config.h"
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace uc {

static bool hex32(const std::string& s, uint32_t& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    unsigned long long x = std::strtoull(s.c_str(), &end, 16);
    if (*end || x > 0xFFFFFFFFull) return false;
    v = uint32_t(x);
    return true;
}

bool parse_config(const std::string& text, Config& out, std::string& error) {
    std::istringstream in(text);
    std::string line;
    int n = 0;
    auto fail = [&](const std::string& m) { error = "line " + std::to_string(n) + ": " + m; return false; };
    while (std::getline(in, line)) {
        ++n;
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ts(line);
        std::vector<std::string> tok;
        for (std::string t; ts >> t;) tok.push_back(t);
        if (tok.empty()) continue;
        uint32_t a = 0;
        if (tok[0] == "entry" || tok[0] == "nofunc") {
            if (tok.size() != 2 || !hex32(tok[1], a)) return fail("expected: " + tok[0] + " <hex address>");
            (tok[0] == "entry" ? out.extra_entries : out.no_functions).insert(a);
        } else if (tok[0] == "hle") {
            if (tok.size() != 3 || !hex32(tok[1], a)) return fail("expected: hle <hex address> <Name>");
            for (char ch : tok[2]) if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') return fail("invalid HLE name '" + tok[2] + "'");
            out.hle[a] = tok[2];
        } else if (tok[0] == "jumptable") {
            JumpTable jt;
            if (tok.size() < 3 || !hex32(tok[1], jt.bctr_addr)) return fail("expected: jumptable <bctr address> <target>...");
            for (size_t i = 2; i < tok.size(); ++i) {
                if (!hex32(tok[i], a)) return fail("bad target address '" + tok[i] + "'");
                jt.targets.push_back(a);
            }
            out.jump_tables.push_back(jt);
        } else {
            return fail("unknown directive '" + tok[0] + "'");
        }
    }
    return true;
}

bool load_config(const std::string& path, Config& out, std::string& error) {
    std::ifstream f(path);
    if (!f) { error = "cannot open " + path; return false; }
    std::stringstream ss; ss << f.rdbuf();
    return parse_config(ss.str(), out, error);
}

}  // namespace uc
