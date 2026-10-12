#include "clock.h"
#include "dispatch.h"
#include "dol.h"
#include "dvdfs.h"
#include "fatal.h"
#include "game_decl.h"
#include "guest_mem.h"
#include "hle/os.h"
#include "hle/os_layout.h"
#include "lowmem.h"
#include "mmio.h"
#include "sched.h"
#include "trace.h"
#include "vi.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern const uc::FnEntry uc_functions[];
extern const size_t uc_function_count;
extern const uint32_t uc_text_begin, uc_text_end;
namespace fs = std::filesystem;

namespace {
struct FifoSink : uc::MmioDevice {   // GX write-gather pipe: M1 discards the command stream (tech-debt, see docs/tech-debt.md)
    uint64_t bytes = 0;
    uint32_t read(uint32_t, int) override { return 0; }
    void write(uint32_t, int size, uint32_t) override { bytes += uint64_t(size); }
};
FifoSink g_fifo;
std::vector<std::unique_ptr<uc::RegFile>> g_regs;

void map_regs(uint32_t base, uint32_t size, const char* name, uint32_t mirror = 0) {   // mirror: second address window of the same registers
    g_regs.push_back(std::make_unique<uc::RegFile>(size));
    uc::mmio_bus().map(base, size, g_regs.back().get(), name);
    if (mirror) uc::mmio_bus().map(mirror, size, g_regs.back().get(), std::string(name) + " (mirror)");
}
void map_hardware() {   // register blocks of the Wii/GameCube as seen at 0xCC000000; plain read-back registers until a trace shows more is needed
    map_regs(0xCC000000, 0x80, "CP");     map_regs(0xCC001000, 0x100, "PE");  map_regs(0xCC002000, 0x100, "VI");
    map_regs(0xCC003000, 0x100, "PI");    map_regs(0xCC004000, 0x100, "MI");  map_regs(0xCC005000, 0x200, "DSP");
    map_regs(0xCC006000, 0x40, "DI", 0xCD006000);     map_regs(0xCC006400, 0x100, "SI", 0xCD006400);  map_regs(0xCC006800, 0x80, "EXI", 0xCD006800);
    map_regs(0xCC006C00, 0x20, "AI", 0xCD006C00);     map_regs(0xCD000000, 0x100, "IPC"); map_regs(0xCD800000, 0x1000, "HW");
    uc::mmio_bus().map(0xCC008000, 0x100, &g_fifo, "GX FIFO");
}

uc::Sched* g_sched;   // for the exit report only

void print_stats() {
    if (g_sched) {
        if (const uc::Thread* t = g_sched->running()) std::fprintf(stderr, "running guest thread %d: r1 %08X lr %08X  interrupts %s\n", t->id, t->ctx.r[1], t->ctx.lr, g_sched->interrupts_enabled() ? "on" : "off");
    }
    auto vi = uc::vi_stats();
    auto d = uc::dvd_stats();
    std::fprintf(stderr, "stats: retraces %llu  dvd opens %llu  dvd reads %llu (%llu bytes)  fifo bytes %llu  unmapped mmio %zu\n",
                 (unsigned long long)vi.retraces, (unsigned long long)d.opens, (unsigned long long)d.reads, (unsigned long long)d.bytes,
                 (unsigned long long)g_fifo.bytes, uc::mmio_bus().unmapped_count());
}

fs::path exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}
std::vector<uint8_t> slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}  // namespace

static int real_main(int argc, char** argv) {
    fs::path game = "S:\\Games\\Umbrella Chronicles\\game";
    int seconds = 0;
    bool log_dvd = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) game = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (a == "--log-dvd") log_dvd = true;
        else if (a == "--log-sleep") uc::os_log_sleep(true);
        else { std::fprintf(stderr, "usage: uc [--game <dir>] [--seconds N] [--log-dvd] [--log-sleep]\n"); return 2; }
    }
    uc::install_crash_handlers();
    uc::set_exit_hook(print_stats);
    for (const char* n : {"identified_sdk.txt", "symbols.txt"}) {   // installed next to the exe, or the project folder of a build tree
        for (const fs::path& d : {exe_dir() / "data", exe_dir() / ".." / ".." / "project"}) if (fs::exists(d / n)) { uc::symbols_load((d / n).string()); break; }
    }

    auto dol_bytes = slurp(game / "DATA" / "sys" / "main.dol");
    auto dol = uc::parse_dol(dol_bytes);
    if (!dol.ok) uc::fatal("cannot load %s: %s", (game / "DATA" / "sys" / "main.dol").string().c_str(), dol.error.c_str());

    uc::guest_mem_init();
    ppc::Ctx boot{};
    boot.mem = uc::guest_mem_base();
    for (const auto& s : dol.image.sections) std::memcpy(boot.mem + (s.address & 0x3FFFFFFFu), dol_bytes.data() + s.file_offset, s.size);   // BSS is already zero
    uint8_t hdr[32] = {};
    auto boot_bin = slurp(game / "DATA" / "sys" / "boot.bin");
    std::memcpy(hdr, boot_bin.data(), std::min<size_t>(32, boot_bin.size()));
    uc::LowMemInfo info;
    info.arena_lo = uc::arena_lo_after_bss(dol.image.bss_address, dol.image.bss_size);
    uc::init_lowmem(boot, hdr, info);

    map_hardware();
    uc::dispatch_init(uc_functions, uc_function_count, uc_text_begin, uc_text_end);
    uc::DvdFs dvd;
    if (!dvd.open_root(game / "DATA" / "files")) uc::fatal("game files not found under %s", (game / "DATA" / "files").string().c_str());
    uc::dvd_hle_init(dvd);
    uc::dvd_log(log_dvd);

    uc::Sched sched(boot.mem);
    g_sched = &sched;
    uc::os_hle_init(sched, boot.mem, 0x817F0000);
    sched.irq_ctx().r[1] = 0x817EFF00;
    uc::vi_hle_init(sched, uc::kViPeriodNtsc, nullptr);

    // The start code runs on the SDK's DefaultThread: __OSThreadInit (802611B8) stores this OSThread in the current-thread word and fills it in.
    const uint32_t kMainThread = 0x805F88D8;
    uint32_t entry = dol.image.entry;
    uc::Thread* mt = sched.create(kMainThread, 16, [entry](ppc::Ctx& c) {
        c.r[1] = 0x817D0000;   // the start code installs its own stack pointer; this one only has to be valid
        uc::call_indirect(c, entry);
    }, &boot);

    if (seconds > 0) {
        std::thread([seconds, &sched] { std::this_thread::sleep_for(std::chrono::seconds(seconds)); std::fprintf(stderr, "guest threads at exit:\n%s", sched.dump_threads().c_str()); uc::trace_dump_all(stderr, 24); print_stats(); std::fflush(stderr); ExitProcess(0); }).detach();
    }
    std::thread([] {   // no-progress watchdog: boot may take a while, then retraces must keep coming
        uint64_t last = 0; int stalled = 0, boot_s = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t n = uc::vi_stats().retraces;
            if (n == 0) { if (++boot_s < 15) continue; }
            else if (n != last) { last = n; stalled = 0; continue; }
            else if (++stalled < 10) continue;
            std::fprintf(stderr, "no progress: retrace count %llu has not changed for a long time\n", (unsigned long long)n);
            uc::trace_dump_all(stderr, 16);
            print_stats();
            std::fflush(stderr);
            ExitProcess(4);
        }
    }).detach();

    sched.run(mt);
    std::fprintf(stderr, "main guest thread exited\n");
    print_stats();
    return 0;
}

int main(int argc, char** argv) {
    try {
        return real_main(argc, argv);
    } catch (const std::exception& e) {
        uc::fatal("startup failed: %s", e.what());
    }
}
