#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace uc {

struct DolSection {
    uint32_t file_offset;
    uint32_t address;
    uint32_t size;
    bool is_text;
};

struct DolImage {
    std::vector<DolSection> sections;  // non-empty sections only, text first
    uint32_t bss_address = 0, bss_size = 0, entry = 0;
};

struct DolResult {
    bool ok = false;
    std::string error;
    DolImage image;
};

DolResult parse_dol(std::span<const uint8_t> file);
std::span<const uint8_t> section_bytes(std::span<const uint8_t> file, const DolSection& s);

}  // namespace uc
