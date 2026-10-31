#pragma once
// Writes stored (uncompressed) ZIP archives for the POSIX update tests, with the Unix mode of every entry under the test's control.
#include <miniz.h>
#include <string>
#include <vector>
namespace zipfix {
struct Entry { std::string name, data; unsigned mode = 0100644; };
inline void put16(std::string& out, unsigned v) { out.push_back(char(v)); out.push_back(char(v >> 8)); }
inline void put32(std::string& out, uint32_t v) { put16(out, v & 0xffff); put16(out, v >> 16); }
inline std::string zip_bytes(const std::vector<Entry>& entries) {
    std::string out, central;
    for (const auto& e : entries) {
        const uint32_t crc = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(e.data.data()), e.data.size()));
        const uint32_t offset = static_cast<uint32_t>(out.size());
        put32(out, 0x04034b50); put16(out, 20); put16(out, 0); put16(out, 0); put16(out, 0); put16(out, 0x21);
        put32(out, crc); put32(out, e.data.size()); put32(out, e.data.size()); put16(out, e.name.size()); put16(out, 0);
        out += e.name; out += e.data;
        put32(central, 0x02014b50); put16(central, 0x0314); put16(central, 20); put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0x21);
        put32(central, crc); put32(central, e.data.size()); put32(central, e.data.size()); put16(central, e.name.size());
        put16(central, 0); put16(central, 0); put16(central, 0); put16(central, 0); put32(central, e.mode << 16); put32(central, offset);
        central += e.name;
    }
    const uint32_t start = static_cast<uint32_t>(out.size());
    out += central;
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, entries.size()); put16(out, entries.size());
    put32(out, central.size()); put32(out, start); put16(out, 0);
    return out;
}
}
