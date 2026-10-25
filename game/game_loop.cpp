#include "game_loop.h"
#include "dispatch.h"
#include <stdexcept>
#include <utility>

namespace {
std::function<void(ppc::Ctx&)> boundary;
thread_local ppc::Ctx* active=nullptr;
}
namespace uc {
void game_loop_set_boundary(std::function<void(ppc::Ctx&)> callback) {
    boundary=std::move(callback);
}
}
void hle_GameLoop(ppc::Ctx& c) {
    if (active==&c) {
        ppc::poll(c); // Preserve the original8003F82C backward-branch safe point.
        return;
    }
    if (active) throw std::logic_error("game loop reentered with another guest context");
    struct Scope {
        explicit Scope(ppc::Ctx& c) { active=&c; }
        ~Scope() { active=nullptr; }
    } scope(c);
    for (;;) {
        if (boundary) boundary(c);
        ppc::op_addi(c,3,22,-22884); // Original8003F670.
        uc::call_indirect(c,0x8003F674);
    }
}
