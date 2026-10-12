#include "hle/os.h"
#include <cstdio>
#include <cstring>

namespace uc {

std::string format_guest(ppc::Ctx& c, uint32_t fmt_addr, int first_gpr) {
    auto cstr = [&](uint32_t a) { std::string s; while (s.size() < 1024) { char ch = char(ppc::rd8(c, a++)); if (!ch) break; s.push_back(ch); } return s; };
    int gpr = first_gpr, fpr = 1, stack_i = 0;
    auto next_int = [&]() -> uint32_t { return gpr <= 10 ? c.r[gpr++] : ppc::rd32(c, c.r[1] + 8 + 4 * uint32_t(stack_i++)); };
    auto next_dbl = [&]() -> double {
        if (fpr <= 8) return c.f[fpr++];
        uint32_t a = c.r[1] + 8 + 4 * uint32_t(stack_i); stack_i += 2;
        return ppc::rdf64(c, a);
    };
    std::string fmt = cstr(fmt_addr), out;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
        size_t j = i + 1;
        while (j < fmt.size() && std::strchr("-+ #0123456789.", fmt[j])) ++j;
        size_t flags_end = j;
        while (j < fmt.size() && std::strchr("lhz", fmt[j])) ++j;   // length modifiers do not change the 32-bit guest ABI
        if (j >= fmt.size()) break;
        std::string flags = fmt.substr(i, flags_end - i);
        char conv = fmt[j], buf[512];
        switch (conv) {
            case '%': out.push_back('%'); break;
            case 'd': case 'i': std::snprintf(buf, sizeof buf, (flags + "d").c_str(), int(next_int())); out += buf; break;
            case 'u': case 'x': case 'X': case 'o': case 'c': std::snprintf(buf, sizeof buf, (flags + conv).c_str(), next_int()); out += buf; break;
            case 'p': std::snprintf(buf, sizeof buf, "0x%08x", next_int()); out += buf; break;
            case 's': { std::string s = cstr(next_int()); std::snprintf(buf, sizeof buf, (flags + "s").c_str(), s.c_str()); out += buf; break; }
            case 'f': case 'g': case 'e': case 'G': case 'E': std::snprintf(buf, sizeof buf, (flags + conv).c_str(), next_dbl()); out += buf; break;
            default: out += fmt.substr(i, j - i + 1); break;
        }
        i = j;
    }
    return out;
}

}  // namespace uc
