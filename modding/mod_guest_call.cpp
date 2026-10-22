#include "mod_guest_call.h"
#include "ppc_rt.h"
#include <atomic>
#include <cstdio>
#include <memory>

namespace uc::mods {
namespace {
std::atomic<const GuestCallBackend*> g_backend{nullptr};
thread_local ppc::Ctx* t_ctx = nullptr;
thread_local int t_depth = 0;
constexpr int kMaxDepth = 4;
}  // namespace

void set_guest_call_backend(const GuestCallBackend* backend) { g_backend.store(backend, std::memory_order_release); }

GuestCallScope::GuestCallScope(ppc::Ctx* ctx) : previous_(t_ctx) { t_ctx = ctx; }
GuestCallScope::~GuestCallScope() { t_ctx = previous_; }

uint32_t guest_call(uint32_t address, const std::vector<uint32_t>& args) {
    const GuestCallBackend* backend = g_backend.load(std::memory_order_acquire);
    if (!backend) throw ApiError("calling game functions is not available");
    if (!t_ctx) throw ApiError("game functions can only be called from the game's own events: on_step, the game events and hook handlers (not from the settings window)");
    if (args.size() > kMaxGuestCallArgs) throw ApiError("a game function call takes at most 8 integer arguments");
    if (!backend->has_function(address)) {
        char text[96];
        std::snprintf(text, sizeof text, "0x%08X is not the start of a game function", address);
        throw ApiError(text);
    }
    if (t_depth >= kMaxDepth) throw ApiError("game function calls are nested too deeply");
    ppc::Ctx& ctx = *t_ctx;
    auto saved = std::make_unique<ppc::Ctx>(ctx);   // ~4.5 KB: kept off the stack
    for (size_t i = 0; i < args.size(); ++i) ctx.r[3 + i] = args[i];
    ++t_depth;
    struct Leave { ~Leave() { --t_depth; } } leave;
    backend->call(ctx, address);
    const uint32_t result = ctx.r[3];
    ctx = *saved;
    return result;
}

}  // namespace uc::mods
