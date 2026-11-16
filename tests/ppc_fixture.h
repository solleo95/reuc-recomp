#pragma once
#include "ppc_rt.h"
#include "guest_mem.h"

// The tests reach the context as the game's guest threads reach theirs: through a pointer to an object the compiler does not know (the scheduler's).
// A context it can see whole (a local of known size) lets it prove that no guest store, c.mem plus a large offset, lands in the context, keep c.mem in a
// register and move a load of one mirror (0xD0000000) above a store to another (0x90000000), though both are the same RAM: the NDK's clang does so in
// test_ppc_mem. With an unknown context every guest store may change c.mem, so the next address is computed after it.
inline ppc::Ctx* volatile g_fixture_context;
struct Fix {
    ppc::Ctx context{};
    ppc::Ctx& c;
    Fix() : c((g_fixture_context = &context, *g_fixture_context)) { uc::guest_mem_init(); c.mem = uc::guest_mem_base(); }
};
