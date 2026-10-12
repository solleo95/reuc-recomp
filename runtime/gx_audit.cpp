#include "gx_audit.h"
#include "trace.h"
#include "gx_vertices.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

namespace {
std::atomic<bool> enabled{false};
std::mutex mutex;
std::map<uint32_t, uc::GxAuditEntry> entries;
bool recording = false;
bool watched(uint32_t address, uint32_t link) {
    // DOL-verified object lifecycle/update dispatch; observe native callback targets without replacing them.
    switch (link) {
    case 0x8003D208: case 0x8003D2AC: case 0x8003D348:
    case 0x8003D408: case 0x8003D430: case 0x8003D4A8:
    case 0x80131740: case 0x801317D4: case 0x801317E8: return true;
    case 0x80131850: case 0x8013188C: case 0x801318A4:
    case 0x801318C4: case 0x801318DC: case 0x80131908: return true;
    case 0x80115348: case 0x8011535C: case 0x80116204: return true;
    default: break;
    }
    if (address >= 0x8028183C && address <= 0x802894B0) return true;
    switch (address) {
    case 0x80039864: case 0x80039C50: case 0x8003A46C: case 0x8003A6AC: case 0x8003A77C:
    case 0x8003CDE0: case 0x80366D3C: case 0x80366E14: case 0x80366E78: case 0x80366FDC:
    case 0x80367AC4: case 0x80367FDC: return true;
    case 0x802F21EC: return true;
    case 0x801028D4: case 0x8011637C: case 0x80116674: return true; // verified startup resource/dialog boundaries
    case 0x802ACAC4: return true; // verified NANDGetHomeDir readiness gate
    case 0x8003D1AC: case 0x8003D250: case 0x8003D2F4: case 0x8003D380:
    case 0x8003A864: case 0x8003A900: return true;
    default: return false;
    }
}
}
namespace uc {
void gx_audit_enable() { enabled.store(true, std::memory_order_relaxed); }
void gx_audit_enter(uint32_t address, const ppc::Ctx& ctx) {
    if (!enabled.load(std::memory_order_relaxed) || !watched(address,ctx.lr)) return;
    std::lock_guard lock(mutex);
    auto& entry = entries[address];
    if (entry.calls == 0) {
        entry.address = address;
        entry.first_caller = ctx.lr >= 4 ? ctx.lr - 4 : 0;
        for (size_t i = 0; i < entry.first_gpr.size(); ++i) entry.first_gpr[i] = ctx.r[i + 3];
        for (size_t i = 0; i < entry.first_fpr.size(); ++i) entry.first_fpr[i] = ctx.f[i + 1];
        if (address==0x8011637C || address==0x80116674) {
            std::fprintf(stderr,"[startup resource] entry %08X caller %08X r3 %08X r4 %08X\n",address,entry.first_caller,ctx.r[3],ctx.r[4]);
            trace_dump(stderr,64);
        }
        if (address==0x802ACAC4 && ctx.mem) {
            const gx::Memory memory{{reinterpret_cast<const std::byte*>(ctx.mem),24u<<20},{reinterpret_cast<const std::byte*>(ctx.mem+0x10000000),64u<<20},{}};
            uint32_t initialized=0;
            for (auto byte : memory.read(ctx.r[13]-25056,4)) initialized=(initialized<<8)|std::to_integer<uint8_t>(byte);
            std::fprintf(stderr,"[save] NANDGetHomeDir caller %08X initialization %u (requires 2)\n",entry.first_caller,initialized);
        }
    }
    ++entry.calls;
    if (address == 0x80288684 && ctx.r[4]) { ++entry.nonempty_lists; entry.list_bytes += ctx.r[4]; }
    if (address == 0x80288528) recording = true;
    if (address == 0x802885D8) recording = false;
}
void gx_audit_fifo_write(int size, uint32_t value) {
    if (!enabled.load(std::memory_order_relaxed)) return;
    if (size != 1 && size != 2 && size != 4) throw std::invalid_argument("invalid GX audit write width");
    std::lock_guard lock(mutex);
    if (!recording) return;
    auto& entry = entries.at(0x80288528);
    entry.recording_bytes += unsigned(size);
    if (entry.calls == 1)
        for (int i = 0; i < size && entry.first_recording_size < entry.first_recording_prefix.size(); ++i)
            entry.first_recording_prefix[entry.first_recording_size++] = uint8_t(value >> ((size - 1 - i) * 8));
}
void gx_audit_recorded_resource(uint32_t address, std::span<const std::byte> bytes) {
    if (!enabled.load(std::memory_order_relaxed) || bytes.empty()) return;
    std::lock_guard lock(mutex);
    auto found = entries.find(0x80288528);
    if (found == entries.end() || found->second.resources.size() == 4) return;
    auto& entry = found->second;
    if (std::any_of(entry.resources.begin(),entry.resources.end(),[address](const auto& resource){return resource.address==address;})) return;
    entry.resources.push_back({address,bytes.size(),{bytes.begin(),bytes.begin()+std::min<size_t>(bytes.size(),4096)}});
}
std::vector<GxAuditEntry> gx_audit_snapshot() {
    std::lock_guard lock(mutex);
    std::vector<GxAuditEntry> snapshot;
    snapshot.reserve(entries.size());
    for (const auto& [address, entry] : entries) snapshot.push_back(entry);
    return snapshot;
}
}
