#include "check.h"
#include "game_loop.h"
#include "dispatch.h"
#include <cstdint>
#include <stdexcept>

namespace {
struct Stop {};
unsigned iterations=0, boundaries=0;
uintptr_t first_stack=0;
void body(ppc::Ctx& c) {
    int stack_marker=0;
    if (!first_stack) first_stack=reinterpret_cast<uintptr_t>(&stack_marker);
    CHECK(reinterpret_cast<uintptr_t>(&stack_marker)==first_stack);
    CHECK(c.r[3]==c.r[22]+uint32_t(-22884));
    ++c.r[7];
    if (++iterations==10000) throw Stop{};
    hle_GameLoop(c); // Translated8003F82C tail transfer; it must return this iteration.
}
}

int main() {
    const uc::FnEntry entries[]={{0x8003F674,body}};
    uc::dispatch_init(entries,1,0x8003F674,0x8003F678);
    ppc::Ctx c{};
    c.r[22]=0x80440000;
    uc::game_loop_set_boundary([](ppc::Ctx& state) {
        CHECK(state.r[7]==boundaries);
        CHECK(state.poll_count==boundaries);
        ++boundaries;
    });
    try { hle_GameLoop(c); CHECK(false); } catch (const Stop&) {}
    CHECK(iterations==10000 && boundaries==10000 && c.r[7]==10000);
    // Exceptions must release the active loop so a later run can enter normally.
    iterations=boundaries=0; c.r[7]=0; c.poll_count=0; first_stack=0;
    try { hle_GameLoop(c); CHECK(false); } catch (const Stop&) {}
    CHECK(iterations==10000 && boundaries==10000);
    uc::game_loop_set_boundary({});
    return 0;
}
