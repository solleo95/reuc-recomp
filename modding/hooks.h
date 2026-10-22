#pragma once
// The hot half of mod hooks, included by the translated game code (game/entry_probe.h, UC_ENTER). Only functions listed in
// modding/generated/hookable.inc (generated from modding/api/uc_api.json) get a hook point; for every other function hook_enter<A>
// compiles to nothing. A hook point is one relaxed load of a pointer that stays null until a mod installs a hook.
// Three kinds: pre (before the body), replace (the body is skipped; the hook sets the return registers) and post (after the function returned).
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ppc { struct Ctx; }

namespace uc::mods {

struct HookChain;   // hook_registry.cpp

enum class HookKind : uint8_t { pre, post, replace };
inline constexpr size_t kHookKinds = 3;

inline constexpr uint32_t kHookable[] = {
#define UC_HOOKABLE(address, name) address,
#include "generated/hookable.inc"
#undef UC_HOOKABLE
};
inline constexpr size_t kHookCount = sizeof(kHookable) / sizeof(kHookable[0]);

constexpr bool is_hookable(uint32_t address) {
    for (const uint32_t a : kHookable) if (a == address) return true;
    return false;
}
constexpr size_t hook_index(uint32_t address) {   // only for hookable addresses
    for (size_t i = 0; i < kHookCount; ++i) if (kHookable[i] == address) return i;
    return 0;
}

inline std::atomic<const HookChain*> g_hook_slots[kHookKinds][kHookCount];   // null: nobody hooked this function

void run_hook_slot(HookKind kind, size_t index, ppc::Ctx& ctx);   // out of line: keeps std::function out of the translated code; registers as a reader so a replaced chain is not freed under it

// The per-step event: the game calls mods_step() once per elapsed game step (not while paused); the mod host installs the callback.
// `nominal` is the number of nominal frames (the game's own 30 Hz step, about 30 per second of game time) the step covers: 0 for a step shorter than one, more for a long one.
// `ctx` is the register file of the guest thread making the call (nullptr in tests): mods may call game functions with it during the step.
using StepCallback = void (*)(void* user, uint32_t nominal, ppc::Ctx* ctx);
inline std::atomic<StepCallback> g_step_callback{nullptr};
inline std::atomic<void*> g_step_user{nullptr};
inline void set_step_callback(StepCallback callback, void* user) {
    g_step_user.store(user, std::memory_order_relaxed);
    g_step_callback.store(callback, std::memory_order_release);
}
inline void mods_step(uint32_t nominal = 1, ppc::Ctx* ctx = nullptr) {
    if (const StepCallback callback = g_step_callback.load(std::memory_order_acquire)) callback(g_step_user.load(std::memory_order_relaxed), nominal, ctx);
}

// Entry hook of a translated function (UC_ENTER): runs the pre hooks, then the replace hooks. True when the function body must be skipped.
template <uint32_t Address>
inline bool hook_enter(ppc::Ctx& ctx) {
    if constexpr (is_hookable(Address)) {
        constexpr size_t index = hook_index(Address);
        if (g_hook_slots[size_t(HookKind::pre)][index].load(std::memory_order_relaxed)) run_hook_slot(HookKind::pre, index, ctx);
        if (g_hook_slots[size_t(HookKind::replace)][index].load(std::memory_order_relaxed)) { run_hook_slot(HookKind::replace, index, ctx); return true; }
    }
    return false;
}

// Declared at the top of a translated function: its destructor runs the post hooks at every return of the function (also after a replace).
template <uint32_t Address>
struct PostScope {
    ppc::Ctx* ctx;
    ~PostScope() {
        if (g_hook_slots[size_t(HookKind::post)][hook_index(Address)].load(std::memory_order_relaxed)) run_hook_slot(HookKind::post, hook_index(Address), *ctx);
    }
};
struct NoPostScope {};
template <uint32_t Address>
inline auto post_scope(ppc::Ctx& ctx) {
    if constexpr (is_hookable(Address)) return PostScope<Address>{&ctx};
    else return NoPostScope{};
}

}  // namespace uc::mods
