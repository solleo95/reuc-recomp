#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace uc {

struct JumpTable {
    uint32_t bctr_addr;
    std::vector<uint32_t> targets;
};

struct Config {
    std::set<uint32_t> extra_entries, no_functions;
    std::set<uint32_t> noopt;             // functions compiled without optimisation (clang optnone): cold code whose register allocation would take tens of seconds
    std::set<uint32_t> isolate;          // functions emitted alone in a file: their register allocation takes tens of seconds, which in a shared file would hold up the whole build
    std::map<uint32_t, std::string> hle;  // guest function address -> HLE name (hle_<name> in the runtime)
    std::vector<JumpTable> jump_tables;
};

// Line format:  entry <hex> | nofunc <hex> | isolate <hex> | noopt <hex> | hle <hex> <Name> | jumptable <bctr hex> <target hex>...   ('#' starts a comment)
bool parse_config(const std::string& text, Config& out, std::string& error);
bool load_config(const std::string& path, Config& out, std::string& error);

}  // namespace uc
