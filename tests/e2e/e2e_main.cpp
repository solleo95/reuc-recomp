#include "dispatch.h"
#include "dol.h"
#include "guest_mem.h"
#include "programs.h"
#include "game_decl.h"
#include <cstdio>

extern const uc::FnEntry uc_functions[];
extern const size_t uc_function_count;
extern const uint32_t uc_text_begin, uc_text_end;

int main() {
    const e2e::Program* prog = e2e::find(E2E_NAME);
    if (!prog) { std::fprintf(stderr, "unknown program %s\n", E2E_NAME); return 1; }
    uc::guest_mem_init();
    ppc::Ctx c{};
    c.mem = uc::guest_mem_base();

    auto dol = prog->build();
    auto parsed = uc::parse_dol(dol);
    if (!parsed.ok) { std::fprintf(stderr, "bad DOL: %s\n", parsed.error.c_str()); return 1; }
    for (const auto& s : parsed.image.sections) std::memcpy(c.mem + (s.address & 0x3FFFFFFF), dol.data() + s.file_offset, s.size);

    uc::dispatch_init(uc_functions, uc_function_count, uc_text_begin, uc_text_end);
    c.r[1] = 0x817FFF00;
    if (prog->init) prog->init(c);
    uc::call_indirect(c, parsed.image.entry);
    if (!prog->check(c)) { std::fprintf(stderr, "e2e %s: check failed (r3=%08X r6=%08X)\n", E2E_NAME, c.r[3], c.r[6]); return 1; }
    std::puts("ok");
    return 0;
}
