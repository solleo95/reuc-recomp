#include "write_trap.h"
#include "trace.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
namespace uc {
namespace {
struct Trap {
    std::string report, go;
    std::vector<uintptr_t> pages, words;   // host page starts, and the watched words (only writes into these are logged)
    bool configured = false, armed = false;
    std::FILE* out = nullptr;
    std::set<std::pair<uintptr_t,uintptr_t>> seen;   // (rip, written address)
    volatile LONG pending = 0;
    uintptr_t pending_page = 0;
    unsigned steps = 0;   // game steps seen by write_trap_step since the first one
    const ppc::Ctx* ctx = nullptr;   // the guest thread that calls write_trap_step: its link register and stack give the guest call chain of a write
};
Trap& trap() { static Trap instance; return instance; }
LONG CALLBACK handler(PEXCEPTION_POINTERS info) {
    auto& t = trap();
    const auto* record = info->ExceptionRecord;
    static const bool reads = std::getenv("UC_READ_TRAP") != nullptr;   // UC_READ_TRAP=1: the pages are made inaccessible and every access (read or write) of a watched word is logged
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && (record->ExceptionInformation[0] == 1 || (reads && record->ExceptionInformation[0] == 0))) {
        const uintptr_t address = record->ExceptionInformation[1], page = address & ~uintptr_t(4095);
        for (const auto watched : t.pages) if (watched == page) {
            const uintptr_t rip = uintptr_t(record->ExceptionAddress);
            bool inside = false;
            for (const auto word : t.words) inside |= address + 8 > word && address < word + 4;
            static const bool all = std::getenv("UC_WRITE_TRAP_ALL") != nullptr;   // log every write, not only each new (rip, address)
            static unsigned logged = 0;
            if (inside && t.out && (all ? logged++ < 4000 : (t.seen.size() < 4000 && t.seen.insert({rip, address}).second))) {
                std::fprintf(t.out, "step %u rva %llX %s %llX thread %lu lr %08X chain", t.steps, (unsigned long long)(rip - uintptr_t(GetModuleHandleW(nullptr))), record->ExceptionInformation[0] == 1 ? "write" : "read", (unsigned long long)address, GetCurrentThreadId(), t.ctx ? t.ctx->lr : 0u);
                if (t.ctx) {   // guest back chain: *(sp) = caller's sp, *(sp + 4) = LR save slot of the caller
                    uint32_t sp = t.ctx->r[1];
                    const auto* mem = reinterpret_cast<const uint8_t*>(t.ctx->mem);
                    for (int depth = 0; depth < 10 && sp >= 0x80000000u && sp < 0x81800000u; ++depth) {
                        uint32_t back, ret;
                        std::memcpy(&back, mem + sp, 4); std::memcpy(&ret, mem + sp + 4, 4);
                        std::fprintf(t.out, " %08X", _byteswap_ulong(ret));
                        back = _byteswap_ulong(back);
                        if (back <= sp) break;
                        sp = back;
                    }
                }
                std::fputc('\n', t.out);
                if (reads) trace_dump(t.out, 3);   // UC_TRACE builds: the guest functions the thread entered last (the reader is the newest)
            }
            DWORD old;
            VirtualProtect(reinterpret_cast<void*>(page), 4096, PAGE_READWRITE, &old);
            t.pending_page = page; InterlockedExchange(&t.pending, 1);
            info->ContextRecord->EFlags |= 0x100;   // single step, then protect again
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    } else if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && InterlockedExchange(&t.pending, 0)) {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(t.pending_page), 4096, std::getenv("UC_READ_TRAP") ? PAGE_NOACCESS : PAGE_READONLY, &old);
        if (t.out) std::fflush(t.out);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
// The views of one RAM block are separate mappings: only the cached 0x80000000/0x90000000 view the translated code uses can be protected.
const std::byte* host_address(const ppc::Ctx& c, uint32_t guest) { return reinterpret_cast<const std::byte*>(c.mem) + guest; }
}
void write_trap_step(const ppc::Ctx& c) {
    auto& t = trap();
    t.ctx = &c;
    ++t.steps;
    if (!t.configured) {
        t.configured = true;
        const char* spec = std::getenv("UC_WRITE_TRAP");
        if (!spec) return;
        std::string text(spec);
        const auto bar = text.find('|');
        if (bar == std::string::npos) return;
        t.report = text.substr(0, bar); t.go = t.report + ".go";
        size_t at = bar + 1;
        while (at < text.size()) {
            auto end = text.find(',', at); if (end == std::string::npos) end = text.size();
            const uint32_t guest = uint32_t(std::strtoul(text.substr(at, end - at).c_str(), nullptr, 16));
            t.words.push_back(reinterpret_cast<uintptr_t>(host_address(c, guest)));
            t.pages.push_back(reinterpret_cast<uintptr_t>(host_address(c, guest)) & ~uintptr_t(4095));
            at = end + 1;
        }
    }
    if (t.pages.empty() || t.armed || !std::filesystem::exists(t.go)) return;
    t.armed = true;
    t.out = std::fopen(t.report.c_str(), "w");
    AddVectoredExceptionHandler(1, handler);
    for (const auto page : t.pages) {
        DWORD old = 0;
        const BOOL ok = VirtualProtect(reinterpret_cast<void*>(page), 4096, std::getenv("UC_READ_TRAP") ? PAGE_NOACCESS : PAGE_READONLY, &old);
        if (t.out) std::fprintf(t.out, "protect %llX ok %d old %lX error %lu\n", (unsigned long long)page, ok, old, ok ? 0ul : GetLastError());
    }
    if (t.out) std::fflush(t.out);
}
}
