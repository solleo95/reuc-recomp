#pragma once
// The hot half of mod hooks, included by the translated game code (game/entry_probe.h, UC_ENTER). Only functions listed in
// modding/generated/hookable.inc (generated from modding/api/uc_api.json) get a hook point; for every other function hook_enter<A>
// compiles to nothing. A hook point is one relaxed load of a pointer that stays null until a mod installs a hook.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ppc { struct Ctx; }

namespace uc::mods {

struct HookChain;   // hook_registry.cpp

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

inline std::atomic<const HookChain*> g_hook_slots[kHookCount];   // null: nobody hooked this function

void run_hook_slot(size_t index, ppc::Ctx& ctx);   // out of line: keeps std::function out of the translated code; registers as a reader so a replaced chain is not freed under it

// The per-step event: the game calls mods_step() once per elapsed game step (not while paused); the mod host installs the callback.
// `nominal` is the number of nominal frames (the game's own 30 Hz step, about 30 per second of game time) the step covers: 0 for a step shorter than one, more for a long one.
using StepCallback = void (*)(void* user, uint32_t nominal);
inline std::atomic<StepCallback> g_step_callback{nullptr};
inline std::atomic<void*> g_step_user{nullptr};
inline void set_step_callback(StepCallback callback, void* user) {
    g_step_user.store(user, std::memory_order_relaxed);
    g_step_callback.store(callback, std::memory_order_release);
}
inline void mods_step(uint32_t nominal = 1) {
    if (const StepCallback callback = g_step_callback.load(std::memory_order_acquire)) callback(g_step_user.load(std::memory_order_relaxed), nominal);
}

template <uint32_t Address>
inline void hook_enter(ppc::Ctx& ctx) {
    if constexpr (is_hookable(Address)) {
        if (g_hook_slots[hook_index(Address)].load(std::memory_order_relaxed)) run_hook_slot(hook_index(Address), ctx);
    }
}

}  // namespace uc::mods
