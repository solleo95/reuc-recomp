#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
namespace uc {
// Diagnostic only (--gx-gaps): classifies guest FIFO commands that reach the runtime outside native HLE calls.
struct GapRow { std::string key; uint64_t count = 0; std::vector<uint32_t> functions; };
void gx_gap_audit_enable(bool);
bool gx_gap_audit_enabled();
// `attribute` returns the guest function performing the write; it is only called for sampled commands.
void gx_gap_audit_write(int size, uint32_t value, bool in_primitive, bool recording, uint32_t (*attribute)());
std::vector<GapRow> gx_gap_audit_rows();
void gx_gap_audit_report(std::FILE* out);
}
