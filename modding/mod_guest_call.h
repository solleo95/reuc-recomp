#pragma once
// Calling translated game functions from a mod. A call needs the register file of the guest thread that is running right now, so it works only
// while the game itself is calling into the mod on that thread: the per-step event (and the game events and handlers it runs) and hook handlers.
#include "api_registry.h"
#include <cstdint>
#include <vector>

namespace ppc { struct Ctx; }

namespace uc::mods {

// What the game provides: whether `address` is the entry of a translated function, and the call itself (runtime/dispatch.h).
struct GuestCallBackend {
    bool (*has_function)(uint32_t address);
    void (*call)(ppc::Ctx& ctx, uint32_t address);
};
void set_guest_call_backend(const GuestCallBackend* backend);   // null: calls are refused

// Marks the calling thread as a guest thread running with `ctx` for the lifetime of the object (nullptr: not a guest thread).
class GuestCallScope {
public:
    explicit GuestCallScope(ppc::Ctx* ctx);
    ~GuestCallScope();
    GuestCallScope(const GuestCallScope&) = delete;
    GuestCallScope& operator=(const GuestCallScope&) = delete;
private:
    ppc::Ctx* previous_;
};

inline constexpr size_t kMaxGuestCallArgs = 8;   // r3..r10

// Runs the function at `address` with integer arguments in r3.. and returns r3. The guest's registers are restored afterwards, so the interrupted code
// does not notice. Throws ApiError outside a guest thread, for an address that is not a function entry, and for calls nested deeper than 4.
uint32_t guest_call(uint32_t address, const std::vector<uint32_t>& args);

}  // namespace uc::mods
