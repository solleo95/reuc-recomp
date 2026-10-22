#include "hook_registry.h"
#include "ppc_rt.h"
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
std::vector<std::unique_ptr<HookChain>> g_retired;   // every chain ever published; freed at exit

void publish(size_t index, std::unique_ptr<HookChain> chain) {
    const HookChain* raw = chain.get();
    g_retired.push_back(std::move(chain));
    g_hook_slots[index].store(raw && !raw->entries.empty() ? raw : nullptr, std::memory_order_release);
}
}  // namespace

void run_hook_chain(const HookChain& chain, ppc::Ctx& ctx) {
    for (const auto& entry : chain.entries) entry.second(ctx);
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
    if (const HookChain* current = g_hook_slots[index].load()) next->entries = current->entries;
    next->entries.emplace_back(owner, std::move(callback));
    publish(index, std::move(next));
    return true;
}

void HookRegistry::remove_owner(const void* owner) {
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < kHookCount; ++i) {
        const HookChain* current = g_hook_slots[i].load();
        if (!current) continue;
        auto next = std::make_unique<HookChain>();
        for (const auto& entry : current->entries) if (entry.first != owner) next->entries.push_back(entry);
        if (next->entries.size() != current->entries.size()) publish(i, std::move(next));
    }
}

size_t HookRegistry::count(uint32_t address) const {
    for (size_t i = 0; i < kHookCount; ++i) {
        if (kHookable[i] != address) continue;
        const HookChain* current = g_hook_slots[i].load();
        return current ? current->entries.size() : 0;
    }
    return 0;
}

void HookRegistry::clear() {
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < kHookCount; ++i) g_hook_slots[i].store(nullptr);
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
