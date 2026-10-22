#include "hook_registry.h"
#include "ppc_rt.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace uc::mods {

struct HookChain {
    std::vector<std::pair<const void*, HookRegistry::Callback>> entries;
};

namespace {
constexpr const char* kNames[] = {
#define UC_HOOKABLE(address, name) name,
#include "generated/hookable.inc"
#undef UC_HOOKABLE
};
std::mutex g_mutex;
std::unique_ptr<HookChain> g_current[kHookCount];   // the chain each slot points to
std::vector<std::unique_ptr<HookChain>> g_retired;   // replaced chains a guest thread may still be walking
std::atomic<int> g_readers{0};                       // guest threads inside run_hook_slot

// Replaced chains are freed once no reader is inside: a reader increments before it loads the slot, the writer stores the slot before it looks at the
// counter (all seq_cst), so a reader that sees only the new chain cannot hold an old one, and one that holds an old chain is still counted.
void reclaim() {
    if (g_readers.load(std::memory_order_seq_cst) == 0) g_retired.clear();
}

void publish(size_t index, std::unique_ptr<HookChain> chain) {
    if (chain && chain->entries.empty()) chain.reset();
    const HookChain* raw = chain.get();
    if (g_current[index]) g_retired.push_back(std::move(g_current[index]));
    g_current[index] = std::move(chain);
    g_hook_slots[index].store(raw, std::memory_order_seq_cst);
    reclaim();
}
}  // namespace

void run_hook_slot(size_t index, ppc::Ctx& ctx) {
    struct Reader {
        Reader() { g_readers.fetch_add(1, std::memory_order_seq_cst); }
        ~Reader() { g_readers.fetch_sub(1, std::memory_order_seq_cst); }
    } reader;
    if (const HookChain* chain = g_hook_slots[index].load(std::memory_order_seq_cst))
        for (const auto& entry : chain->entries) entry.second(ctx);
}

std::optional<uint32_t> HookRegistry::address_of(std::string_view name) {
    for (size_t i = 0; i < kHookCount; ++i) if (name == kNames[i]) return kHookable[i];
    return std::nullopt;
}

std::vector<std::string> HookRegistry::names() {
    return std::vector<std::string>(std::begin(kNames), std::end(kNames));
}

bool HookRegistry::add(uint32_t address, const void* owner, Callback callback) {
    size_t index = kHookCount;
    for (size_t i = 0; i < kHookCount; ++i) if (kHookable[i] == address) index = i;
    if (index == kHookCount || !callback) return false;
    std::lock_guard lock(g_mutex);
    auto next = std::make_unique<HookChain>();
    if (g_current[index]) next->entries = g_current[index]->entries;
    next->entries.emplace_back(owner, std::move(callback));
    publish(index, std::move(next));
    return true;
}

void HookRegistry::remove_owner(const void* owner) {
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < kHookCount; ++i) {
        const HookChain* current = g_current[i].get();
        if (!current) continue;
        auto next = std::make_unique<HookChain>();
        for (const auto& entry : current->entries) if (entry.first != owner) next->entries.push_back(entry);
        if (next->entries.size() != current->entries.size()) publish(i, std::move(next));
    }
}

size_t HookRegistry::count(uint32_t address) const {
    for (size_t i = 0; i < kHookCount; ++i) {
        if (kHookable[i] != address) continue;
        std::lock_guard lock(g_mutex);
        return g_current[i] ? g_current[i]->entries.size() : 0;
    }
    return 0;
}

void HookRegistry::clear() {
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < kHookCount; ++i) {
        g_hook_slots[i].store(nullptr, std::memory_order_seq_cst);
        if (g_current[i]) g_retired.push_back(std::move(g_current[i]));
    }
    reclaim();
}

size_t HookRegistry::retired_chains() const {
    std::lock_guard lock(g_mutex);
    return g_retired.size();
}

HookRegistry& hook_registry() {
    static HookRegistry instance;
    return instance;
}

HookRegs read_hook_regs(const ppc::Ctx& ctx) {
    HookRegs regs;
    for (int i = 0; i < 8; ++i) { regs.r[i] = ctx.r[3 + i]; regs.f[i] = ctx.f[1 + i]; }
    return regs;
}

void write_hook_regs(ppc::Ctx& ctx, const HookRegs& regs) {
    for (int i = 0; i < 8; ++i) { ctx.r[3 + i] = regs.r[i]; ctx.f[1 + i] = regs.f[i]; }
}

}  // namespace uc::mods
