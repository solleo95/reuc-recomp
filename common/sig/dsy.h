#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace uc {

struct DsyEntry {
    uint32_t checksum;
    uint32_t size;
    std::string name;
};

bool parse_dsy(std::span<const uint8_t> bytes, std::vector<DsyEntry>& out, std::string& error);
bool load_dsy(const std::string& path, std::vector<DsyEntry>& out, std::string& error);

// One step of the signature checksum: rotate the running sum left by 17, then xor in the masked instruction word.
uint32_t dsy_step(uint32_t sum, uint32_t word);

}  // namespace uc
