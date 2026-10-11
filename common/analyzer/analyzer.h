#pragma once
#include "analyzer/config.h"
#include "dol.h"
#include <map>
#include <set>
#include <span>
#include <vector>

namespace uc {

struct Image {
    std::span<const uint8_t> file;
    const DolImage* dol;
    const DolSection* section(uint32_t addr) const;
    bool word(uint32_t addr, uint32_t& out) const;  // 4-aligned big-endian read from any section
    bool in_text(uint32_t addr) const;
};

struct Function {
    uint32_t entry, end;  // [entry, end)
};

struct Analysis {
    std::vector<Function> functions;  // sorted by entry
    std::set<uint32_t> entries;
    std::vector<JumpTable> jump_tables;
    std::vector<uint32_t> unresolved_bctr;  // bctr that looks like a switch (mtctr fed by lwzx) but whose table was not found
    std::vector<uint32_t> indirect_tail;    // other non-linking bctr: indirect tail calls (virtual-call thunks)
    // Function entry -> addresses inside it that other functions branch to (extra entry points into the same body).
    std::map<uint32_t, std::set<uint32_t>> secondary;
    bool is_secondary(uint32_t addr) const {
        for (const auto& [owner, s] : secondary) if (s.count(addr)) return true;
        return false;
    }
    const Function* find_containing(uint32_t addr) const;
    bool is_entry(uint32_t addr) const { return entries.count(addr) != 0; }
};

Analysis analyze(const Image& img, const Config& cfg);

}  // namespace uc
