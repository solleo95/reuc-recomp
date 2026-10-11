#pragma once
#include "analyzer/analyzer.h"
#include <string>
#include <utility>
#include <vector>

namespace uc {

struct EmitStats {
    size_t functions = 0, instructions = 0, invalid = 0, traps_midfunc = 0, unresolved_bctr = 0, oe_traps = 0, unsupported = 0;
    size_t indirect_tail = 0, secondary_entries = 0, fallthrough_tail = 0;
};

struct Program {
    Image img{};
    Analysis an;
    Config cfg;
};

struct Output {
    std::vector<std::pair<std::string, std::string>> files;  // name -> content
};

std::string emit_insn(const Program& p, const Function& f, uint32_t pc, uint32_t word, EmitStats& st);
std::string emit_function(const Program& p, const Function& f, EmitStats& st);
Output emit_program(const Program& p, size_t funcs_per_shard, EmitStats& st);

}  // namespace uc
