#pragma once
#include "analyzer/analyzer.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uc {

struct EmitStats {
    size_t functions = 0, instructions = 0, invalid = 0, traps_midfunc = 0, unresolved_bctr = 0, oe_traps = 0, unsupported = 0;
    size_t indirect_tail = 0, secondary_entries = 0, fallthrough_tail = 0;
    size_t cr_dead_compares = 0, cr_fused_branches = 0, cr_dropped_records = 0;   // condition-register writes that were never read
};

struct Program {
    Image img{};
    Analysis an;
    Config cfg;
    bool cr_opt = true;   // drop dead CR writes and fuse compare+branch (translator --no-cr-opt turns it off)
};

struct Output {
    std::vector<std::pair<std::string, std::string>> files;  // name -> content
};

// Resume a parked SDK call through the guest LR chain after its native stack unwinds.
void add_resume_entries(Program& p);

// Condition-register liveness. out[function entry][i] = CR fields (bit f = cr<f>) that may still be read after instruction i of the function.
struct CrLive {
    std::unordered_map<uint32_t, std::vector<uint8_t>> out;
};
CrLive analyze_cr(const Program& p);

// What the emitter may drop or fuse for one instruction (from CrLive); the default emits every CR write.
struct FusedCmp { std::string lhs, rhs; };   // the compare feeding the next conditional branch, as C++ operands
struct CrHints {
    bool drop_rc = false;                 // the record form's CR write is never read
    const FusedCmp* fused = nullptr;      // a conditional branch tests this compare directly instead of reading CR
};

std::string emit_insn(const Program& p, const Function& f, uint32_t pc, uint32_t word, EmitStats& st, const CrHints* hints = nullptr);
std::string emit_function(const Program& p, const Function& f, EmitStats& st, const CrLive* live = nullptr);
Output emit_program(const Program& p, size_t funcs_per_shard, EmitStats& st);

}  // namespace uc
