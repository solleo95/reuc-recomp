#include "check.h"
#include "hook_registry.h"
#include "ppc_rt.h"
#include <atomic>
#include <memory>
#include <set>
#include <thread>
#include <type_traits>

using namespace uc::mods;

static_assert(is_hookable(0x8006C268u) && is_hookable(0x800676A0u));
static_assert(!is_hookable(0x80000000u) && !is_hookable(0u));
static_assert(kHookCount >= 2);

static ppc::Ctx make_ctx() {
    static ppc::Ctx c;
    c = {};
    return c;
}

static void hook_slot_null_by_default() {
    hook_registry().clear();
    for (size_t i = 0; i < kHookCount; ++i) CHECK(g_hook_slots[0][i].load() == nullptr);
    ppc::Ctx c = make_ctx();
    hook_enter<0x8006C268u>(c);   // nothing installed: must be a harmless no-op
    hook_enter<0x80000000u>(c);   // not hookable: compiles to nothing
}

static void hook_installed_called_with_context() {
    hook_registry().clear();
    int owner = 0, calls = 0;
    uint32_t seen_r3 = 0;
    CHECK(hook_registry().add(0x8006C268u, &owner, [&](ppc::Ctx& c) { ++calls; seen_r3 = c.r[3]; c.r[4] = 99; }));
    ppc::Ctx c = make_ctx();
    c.r[3] = 5;
    hook_enter<0x8006C268u>(c);
    CHECK(calls == 1 && seen_r3 == 5 && c.r[4] == 99);
    hook_enter<0x800676A0u>(c);   // another hookable function: not this callback
    CHECK(calls == 1);
    hook_registry().remove_owner(&owner);
    hook_enter<0x8006C268u>(c);
    CHECK(calls == 1 && g_hook_slots[0][hook_index(0x8006C268u)].load() == nullptr);
}

static void callbacks_run_in_install_order_and_owners_are_independent() {
    hook_registry().clear();
    int a = 0, b = 0;
    std::string order;
    CHECK(hook_registry().add(0x8006C268u, &a, [&](ppc::Ctx&) { order += "a"; }));
    CHECK(hook_registry().add(0x8006C268u, &b, [&](ppc::Ctx&) { order += "b"; }));
    CHECK(hook_registry().add(0x8006C268u, &a, [&](ppc::Ctx&) { order += "A"; }));
    ppc::Ctx c = make_ctx();
    hook_enter<0x8006C268u>(c);
    CHECK(order == "abA" && hook_registry().count(0x8006C268u) == 3);
    hook_registry().remove_owner(&a);
    order.clear();
    hook_enter<0x8006C268u>(c);
    CHECK(order == "b" && hook_registry().count(0x8006C268u) == 1);
    hook_registry().clear();
}

static void non_hookable_address_refused() {
    hook_registry().clear();
    int owner = 0;
    CHECK(!hook_registry().add(0x80000000u, &owner, [](ppc::Ctx&) {}));
    CHECK(!hook_registry().add(0x8006C268u, &owner, nullptr));
    CHECK(hook_registry().count(0x80000000u) == 0);
}

static void names_and_addresses() {
    CHECK(HookRegistry::address_of("GameWeaponUpdate") == 0x8006C268u && HookRegistry::address_of("PlayerEventHandler") == 0x800676A0u);
    CHECK(!HookRegistry::address_of("Nothing"));
    const auto names = HookRegistry::names();
    CHECK(names.size() == kHookCount);
}

static void hook_index_stable() {
    std::set<size_t> seen;
    for (size_t i = 0; i < kHookCount; ++i) {
        const size_t index = hook_index(kHookable[i]);
        CHECK(index == i && index < kHookCount && seen.insert(index).second);
    }
}

static void regs_round_trip() {
    ppc::Ctx c = make_ctx();
    for (int i = 0; i < 8; ++i) { c.r[3 + i] = 100 + uint32_t(i); c.f[1 + i] = 0.5 * i; }
    c.r[2] = 7; c.r[11] = 8; c.f[0] = 1.0; c.f[9] = 2.0;   // outside the window: untouched
    HookRegs regs = read_hook_regs(c);
    CHECK(regs.r[0] == 100 && regs.r[7] == 107 && regs.f[1] == 0.5 && regs.f[7] == 3.5);
    regs.r[0] = 1; regs.f[7] = -1.0;
    write_hook_regs(c, regs);
    CHECK(c.r[3] == 1 && c.f[8] == -1.0 && c.r[2] == 7 && c.r[11] == 8 && c.f[0] == 1.0 && c.f[9] == 2.0);
}

// Chains are replaced while the guest thread runs them (mods are switched on and off live): a chain that is still being walked is never freed, and
// replaced chains do not pile up for ever.
static void replaced_chains_are_freed_and_never_under_a_running_hook() {
    hook_registry().clear();
    std::atomic<bool> stop{false};
    std::atomic<int> calls{0}, bad{0};
    std::thread guest([&] {
        ppc::Ctx c = make_ctx();
        while (!stop) { hook_enter<0x8006C268u>(c); }
    });
    std::vector<int> owners(8);
    for (int round = 0; round < 3000; ++round) {
        for (auto& owner : owners) {
            auto payload = std::make_shared<int>(7);   // freed with the chain: a callback running on a freed chain would read garbage
            hook_registry().add(0x8006C268u, &owner, [payload, &calls, &bad](ppc::Ctx&) { if (*payload != 7) ++bad; ++calls; });
        }
        for (auto& owner : owners) hook_registry().remove_owner(&owner);
    }
    stop = true;
    guest.join();
    CHECK(bad == 0 && calls > 0);
    // While the guest thread spins inside hooks on a loaded machine nothing may be freed for a while (a chain is freed once no reader is inside); with the
    // reader gone one more replacement frees what is left.
    hook_registry().add(0x8006C268u, &owners[0], [](ppc::Ctx&) {});
    hook_registry().remove_owner(&owners[0]);
    CHECK(hook_registry().retired_chains() < 64);   // not one per add/remove ever made (48000)
}

// What UC_ENTER does at the top of a translated function: a post scope first (its destructor runs at every return), then the entry hook, which may skip the body.
static void translated_function(ppc::Ctx& c, bool& body_ran) {
    auto post = post_scope<0x8006C268u>(c);
    if (hook_enter<0x8006C268u>(c)) return;
    body_ran = true;
    c.r[3] = 5;   // the function's return value
}

static void post_and_replace_hooks() {
    hook_registry().clear();
    ppc::Ctx c = make_ctx();
    bool ran = false;
    translated_function(c, ran);
    CHECK(ran && c.r[3] == 5);                                   // nothing installed: the function is untouched
    static_assert(std::is_empty_v<decltype(post_scope<0x80000000u>(std::declval<ppc::Ctx&>()))>);   // a function that cannot be hooked pays nothing
    int owner = 0, pre_calls = 0, post_calls = 0;
    uint32_t post_saw_r3 = 0;
    CHECK(hook_registry().add(0x8006C268u, &owner, [&](ppc::Ctx&) { ++post_calls; post_saw_r3 = c.r[3]; c.r[3] += 1; }, HookKind::post));
    c = make_ctx(); ran = false;
    translated_function(c, ran);
    CHECK(ran && post_calls == 1 && post_saw_r3 == 5 && c.r[3] == 6);   // the post hook sees the return value and may change it
    CHECK(hook_registry().add(0x8006C268u, &owner, [&](ppc::Ctx&) { ++pre_calls; }));
    CHECK(hook_registry().add(0x8006C268u, &owner, [&](ppc::Ctx& ctx) { ctx.r[3] = 77; }, HookKind::replace));
    c = make_ctx(); ran = false;
    translated_function(c, ran);
    CHECK(!ran && pre_calls == 1 && post_calls == 2 && post_saw_r3 == 77 && c.r[3] == 78);   // pre runs, the body does not, post still runs after the replacement
    CHECK(hook_registry().count(0x8006C268u, HookKind::post) == 1 && hook_registry().count(0x8006C268u, HookKind::replace) == 1 && hook_registry().count(0x8006C268u) == 1);
    hook_registry().remove_owner(&owner);                         // one owner drops all three kinds
    c = make_ctx(); ran = false;
    translated_function(c, ran);
    CHECK(ran && c.r[3] == 5 && post_calls == 2);
    CHECK(!hook_registry().add(0x80000000u, &owner, [](ppc::Ctx&) {}, HookKind::replace));   // not hookable
}

int main() {
    post_and_replace_hooks();
    replaced_chains_are_freed_and_never_under_a_running_hook();
    hook_slot_null_by_default(); hook_installed_called_with_context(); callbacks_run_in_install_order_and_owners_are_independent();
    non_hookable_address_refused(); names_and_addresses(); hook_index_stable(); regs_round_trip();
    std::puts("ok");
}
