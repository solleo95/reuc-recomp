#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
namespace uc {
// Diagnostic only (--gx-gaps): classifies guest FIFO commands that reach the runtime outside native HLE calls.
struct GapRow { std::string key; uint64_t count = 0; std::vector<uint32_t> functions; };
extern bool gx_gap_audit_on;   // what gx_gap_audit_enabled returns, readable inline on the FIFO write path
void gx_gap_audit_enable(bool);
inline bool gx_gap_audit_enabled() { return gx_gap_audit_on; }
// `attribute` returns the guest function performing the write; it is only called for sampled commands.
void gx_gap_audit_write(int size, uint32_t value, bool in_primitive, bool recording, uint32_t (*attribute)());
std::vector<GapRow> gx_gap_audit_rows();
void gx_gap_audit_report(std::FILE* out);
}
