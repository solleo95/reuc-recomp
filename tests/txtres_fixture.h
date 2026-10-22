#pragma once
// A synthetic text file with the layout of the game's mes_<lang>.txtres (header, table, records, a pool that starts with a leftover block and shares strings).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace txtres_fixture {
struct Rec { uint32_t id; std::string voice; std::vector<std::string> lines; bool share_text_with_previous = false; };

inline void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) d[at + size_t(i)] = uint8_t(v >> (24 - 8 * i)); }
inline uint32_t get32(const std::vector<uint8_t>& d, size_t at) { return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3]; }

// Header, table, records, then a pool that begins with a leftover block (like the Russian file) and shares strings where asked.
inline std::vector<uint8_t> make_txtres(const std::vector<Rec>& records) {
    const uint32_t count = uint32_t(records.size());
    std::vector<uint8_t> d(0x10 + 4 * count + 20 * count, 0);
    std::memcpy(d.data(), "TRES", 4);
    put32(d, 4, 0x00010000); put32(d, 8, count); put32(d, 12, 0x10);
    const auto append = [&](const std::string& text) { const uint32_t at = uint32_t(d.size()); d.insert(d.end(), text.begin(), text.end()); d.push_back(0); return at; };
    append("leftover block");
    uint32_t previous_text = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t record = 0x10 + 4 * count + 20 * i;
        put32(d, 0x10 + 4 * i, uint32_t(record));
        put32(d, record, records[i].id);
        put32(d, record + 4, records[i].voice.empty() ? 0 : append(records[i].voice));
        put32(d, record + 8, uint32_t(records[i].lines.size()));
        uint32_t text = 0;
        if (records[i].share_text_with_previous) text = previous_text;
        else for (size_t l = 0; l < records[i].lines.size(); ++l) { const uint32_t at = append(records[i].lines[l]); if (l == 0) text = at; }
        put32(d, record + 12, text);
        previous_text = text;
    }
    return d;
}

}  // namespace txtres_fixture
