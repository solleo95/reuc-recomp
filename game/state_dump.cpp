#include "state_dump.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
namespace uc {
namespace {
constexpr size_t kMem1 = 24u << 20, kMem2 = 64u << 20;
struct Dump {
    std::string prefix, go;
    std::vector<double> times;
    size_t next = 0;
    double elapsed = 0;
    bool enabled = false, armed = false;
    Dump() {
        const char* spec = std::getenv("UC_STATE_DUMP");
        if (!spec) return;
        std::string text(spec);
        const auto bar = text.find('|');
        if (bar == std::string::npos) return;
        prefix = text.substr(0, bar); go = prefix + ".go";
        for (size_t at = bar + 1; at < text.size();) {
            auto end = text.find(',', at); if (end == std::string::npos) end = text.size();
            times.push_back(std::strtod(text.substr(at, end - at).c_str(), nullptr));
            at = end + 1;
        }
        enabled = !times.empty();
    }
};
Dump& dump() { static Dump instance; return instance; }
}
void state_dump_step(const ppc::Ctx& c, float timestep) {
    auto& d = dump();
    if (!d.enabled || d.next >= d.times.size() || !c.mem) return;
    if (!d.armed) { if (!std::filesystem::exists(d.go)) return; d.armed = true; }
    d.elapsed += timestep;
    if (d.elapsed < d.times[d.next]) return;
    char name[32]; std::snprintf(name, sizeof name, "%.2f", d.times[d.next]);
    std::FILE* out = std::fopen((d.prefix + "_" + name + ".bin").c_str(), "wb");
    if (out) {
        std::fwrite(c.mem, 1, kMem1, out);
        std::fwrite(c.mem + 0x10000000, 1, kMem2, out);
        std::fclose(out);
    }
    ++d.next;
}
}
