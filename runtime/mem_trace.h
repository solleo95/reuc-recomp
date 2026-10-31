#pragma once
// Guest memory access tracer for finding loops and calls that can run in parallel (docs/superpowers/plans/2026-10-31-performance-roadmap.md, Part C).
// Built only with -DUC_TRACE_MEM=ON (a separate build folder): every guest load and store is recorded for the innermost open call of a tracked function.
// UC_TRACE_MEM_FUNCS=80383200,8037A5CC,...  the functions whose calls are recorded (hex, comma separated)
// UC_TRACE_MEM_FROM=<retrace> UC_TRACE_MEM_FRAMES=<n>  the window, in VI retraces (default: from 0 for 120)
// UC_TRACE_MEM_FLAG=<file>  the window opens at the first retrace after that file exists (UC_TRACE_MEM_FROM is then ignored)
// UC_TRACE_MEM_OUT=<file>  the result (default mem-trace.txt), read by scripts/loop_independence.py
#include <cstdint>
namespace ppc { struct Ctx; }
namespace uc::mem_trace {
#ifdef UC_TRACE_MEM
void read(uint32_t ea, uint32_t size);
void write(uint32_t ea, uint32_t size);
// An HLE function ran inside the open calls: its own accesses are not seen, so its pointer arguments (r3..r6) are recorded as possibly read and written.
void hle(const char* name, const ppc::Ctx& c);
struct Scope {
    bool on = false;
    Scope() = default;
    explicit Scope(bool on) : on(on) {}
    Scope(Scope&& other) noexcept : on(other.on) { other.on = false; }
    Scope(const Scope&) = delete;
    ~Scope();
};
Scope enter(uint32_t function, const ppc::Ctx& c);
void flush();
#endif
}
