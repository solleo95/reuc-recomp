#pragma once
#include "hooks.h"
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uc::mods {

// What a hook callback sees: r3..r10 and f1..f8 of the guest at function entry (the calling convention's argument registers).
// Changes written to them go back into the guest before the function body runs.
struct HookRegs {
    uint32_t r[8] = {};   // r3..r10
    double f[8] = {};     // f1..f8
};

// Installs and removes callbacks at the hook points of hooks.h. Chains are published copy-on-write so the guest thread never waits;
// replaced chains are freed as soon as no guest thread is inside a hook (a thread that is walking one is counted).
class HookRegistry {
public:
    using Callback = std::function<void(ppc::Ctx&)>;

    static std::optional<uint32_t> address_of(std::string_view name);   // hookable function name -> guest address
    static std::vector<std::string> names();

    bool add(uint32_t address, const void* owner, Callback callback, HookKind kind = HookKind::pre);   // false when the address is not hookable
    void remove_owner(const void* owner);
    size_t count(uint32_t address, HookKind kind = HookKind::pre) const;  // callbacks of that kind installed at the address
    void clear();                                                        // everything (tests)
    size_t retired_chains() const;                                       // replaced chains not freed yet (tests)
};

HookRegistry& hook_registry();

// ppc::Ctx <-> HookRegs
HookRegs read_hook_regs(const ppc::Ctx& ctx);
void write_hook_regs(ppc::Ctx& ctx, const HookRegs& regs);

}  // namespace uc::mods
