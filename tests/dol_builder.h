#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

inline void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    v[off] = uint8_t(x >> 24); v[off + 1] = uint8_t(x >> 16);
    v[off + 2] = uint8_t(x >> 8); v[off + 3] = uint8_t(x);
}

// Text section 0 at 0x80003100 holds `code`; optional data section 0 at 0x80400000 holds `data`.
inline std::vector<uint8_t> make_dol(const std::vector<uint32_t>& code, const std::vector<uint8_t>& data = {}) {
    const uint32_t text_addr = 0x80003100, data_addr = 0x80400000;
    std::vector<uint8_t> v(0x100 + code.size() * 4 + data.size(), 0);
    put32(v, 0x00, 0x100);
    put32(v, 0x48, text_addr);
    put32(v, 0x90, uint32_t(code.size() * 4));
    put32(v, 0xE0, text_addr);
    for (size_t i = 0; i < code.size(); ++i) put32(v, 0x100 + i * 4, code[i]);
    if (!data.empty()) {
        put32(v, 0x1C, uint32_t(0x100 + code.size() * 4));
        put32(v, 0x64, data_addr);
        put32(v, 0xAC, uint32_t(data.size()));
        for (size_t i = 0; i < data.size(); ++i) v[0x100 + code.size() * 4 + i] = data[i];
    }
    return v;
}
