---

### Task 7: Video retrace (VI) HLE and shared periodic schedule (TDD)

**Files:**
- Create: `runtime/vi.h`, `runtime/hle/vi.cpp`, `tests/test_vi.cpp`
- Modify: `runtime/clock.h`, `tests/test_clock.cpp`, `runtime/hle/os_misc.cpp`, `runtime/CMakeLists.txt`, `tests/CMakeLists.txt`, `project/hle_map.txt`

**Interfaces:**
- Consumes: `uc::Sched::post_at/wake_all/block_on`, `uc::os_sched()`, `uc::call_guest`, `uc::now_ticks`.
- Produces:
  - `uint64_t uc::next_periodic(uint64_t scheduled, uint64_t period, uint64_t now)` (in `clock.h`): the next slot of `scheduled + k*period` after `now`; slots missed while the scheduler was busy are skipped, the phase is kept.
  - `constexpr uint64_t uc::kViPeriodNtsc = kTimebaseHz * 1001 / 60000;` (59.94 Hz), `uc::kViPeriodPal = kTimebaseHz / 50;`
  - `void uc::vi_hle_init(Sched& s, uint64_t period_ticks, std::function<void()> on_retrace);` (starts the periodic retrace event; call after `os_hle_init`), `struct uc::ViStats { uint64_t retraces; uint32_t current_fb, next_fb; };` `ViStats uc::vi_stats();`
  - HLE: `hle_VIInit hle_VIConfigure hle_VIFlush hle_VISetNextFrameBuffer hle_VISetBlack hle_VIGetTvFormat hle_VIGetRetraceCount hle_VIWaitForRetrace hle_VISetPreRetraceCallback hle_VISetPostRetraceCallback`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_clock.cpp` (before `return 0;`):
```cpp
    // periodic schedule: the phase is kept, missed slots are skipped, no drift
    CHECK(uc::next_periodic(1000, 100, 1050) == 1100);
    CHECK(uc::next_periodic(1000, 100, 1100) == 1200);
    CHECK(uc::next_periodic(1000, 100, 1350) == 1400);
    uint64_t t = 1000;
    for (int i = 0; i < 1000; ++i) t = uc::next_periodic(t, 100, t + 3);
    CHECK(t == 1000 + 1000 * 100);
```
`tests/test_vi.cpp`:
```cpp
#include "check.h"
#include "clock.h"
#include "dispatch.h"
#include "hle/os.h"
#include "ppc_fixture.h"
#include "sched.h"
#include "vi.h"
using namespace ppc;

void hle_VIWaitForRetrace(Ctx&); void hle_VISetNextFrameBuffer(Ctx&); void hle_VIGetRetraceCount(Ctx&);
void hle_VISetPostRetraceCallback(Ctx&); void hle_VIConfigure(Ctx&);

static int g_cb_calls = 0; static uint32_t g_cb_arg = 0;
static void post_cb(Ctx& c) { ++g_cb_calls; g_cb_arg = c.r[3]; }

int main() {
    Fix f; Ctx& cc = f.c;
    uc::Sched s(cc.mem);
    static const uc::FnEntry table[] = {{0x80100000, post_cb}};
    uc::dispatch_init(table, 1, 0x80100000, 0x80100004);
    uc::os_hle_init(s, cc.mem, 0x817F0000);
    s.irq_ctx().r[1] = 0x817EFF00;
    int hook_calls = 0;
    uc::vi_hle_init(s, uc::ms_to_ticks(10), [&] { ++hook_calls; });
    uc::Thread* m = s.create(0, 16, [&](Ctx& c) {
        c.r[3] = 0x80100000; hle_VISetPostRetraceCallback(c); CHECK(c.r[3] == 0);   // returns the previous callback
        c.r[3] = 0x80400000; hle_VISetNextFrameBuffer(c);
        CHECK(uc::vi_stats().current_fb == 0);          // a new frame buffer only goes live at the next retrace
        uint64_t a = uc::now_ticks();
        for (int i = 0; i < 5; ++i) hle_VIWaitForRetrace(c);
        uint64_t ms = uc::ticks_to_ms(uc::now_ticks() - a);
        CHECK(ms >= 40 && ms < 400);                    // five retraces at 10 ms
        CHECK(uc::vi_stats().current_fb == 0x80400000);
        hle_VIGetRetraceCount(c);
        CHECK(c.r[3] >= 5);
        CHECK(hook_calls >= 5 && g_cb_calls >= 5 && g_cb_arg >= 5);   // callback gets the retrace count in r3
        // a PAL render mode (viTVmode >> 2 == 1) switches the period to 50 Hz
        wr32(c, 0x80410000, 1u << 2);
        c.r[3] = 0x80410000; hle_VIConfigure(c);
        CHECK(uc::vi_stats().tv_mode == 1);
    });
    s.run(m);
    return 0;
}
```
Add `tv_mode` to `ViStats`. Add `test_vi` to the `test_support` loop. Run: `scripts\dev.bat` — Expected: FAIL (`vi.h` / `next_periodic` missing).

- [ ] **Step 2: Implement**

`runtime/clock.h`: append inside the namespace:
```cpp
// Next point of the schedule `scheduled + k*period` that lies after `now`; slots missed while busy are skipped, the phase is kept.
inline uint64_t next_periodic(uint64_t scheduled, uint64_t period, uint64_t now) {
    uint64_t next = scheduled + period;
    if (next <= now) next += ((now - next) / period + 1) * period;
    return next;
}
```
`runtime/hle/os_misc.cpp`: in `arm()` replace the inline catch-up arithmetic by `arm(alarm, next_periodic(at, period, now_ticks()), period, handler, gen);`.

`runtime/vi.h`:
```cpp
#pragma once
#include "clock.h"
#include "sched.h"
#include <functional>

namespace uc {
constexpr uint64_t kViPeriodNtsc = kTimebaseHz * 1001 / 60000;   // 59.94 Hz
constexpr uint64_t kViPeriodPal = kTimebaseHz / 50;

struct ViStats { uint64_t retraces = 0; uint32_t current_fb = 0, next_fb = 0, tv_mode = 0; };

void vi_hle_init(Sched& s, uint64_t period_ticks, std::function<void()> on_retrace);   // after os_hle_init
ViStats vi_stats();
}
```
`runtime/hle/vi.cpp`:
```cpp
#include "vi.h"
#include "guest_call.h"
#include "hle/os.h"
#include <atomic>

using namespace uc;

namespace {
constexpr uint32_t kViKey = 0xF4000001u;
struct Vi {
    std::atomic<uint64_t> retraces{0};
    uint32_t cur_fb = 0, next_fb = 0, pre_cb = 0, post_cb = 0, tv = 0;
    uint64_t period = kViPeriodNtsc;
    std::function<void()> hook;
} g;

void arm(uint64_t at) {
    os_sched().post_at(at, [at] {
        Sched& s = os_sched();
        uint64_t n = ++g.retraces;
        g.cur_fb = g.next_fb;                                   // a frame buffer set during the last frame goes live now
        arm(next_periodic(at, g.period, now_ticks()));          // re-arm first: callbacks may take long
        if (g.pre_cb) call_guest(s.irq_ctx(), g.pre_cb, {uint32_t(n)});
        if (g.hook) g.hook();
        if (g.post_cb) call_guest(s.irq_ctx(), g.post_cb, {uint32_t(n)});
        s.wake_all(kViKey);
    });
}
}  // namespace

namespace uc {
void vi_hle_init(Sched&, uint64_t period_ticks, std::function<void()> on_retrace) {
    g.period = period_ticks;
    g.hook = std::move(on_retrace);
    arm(now_ticks() + period_ticks);
}
ViStats vi_stats() { return {g.retraces.load(), g.cur_fb, g.next_fb, g.tv}; }
}  // namespace uc

void hle_VIInit(ppc::Ctx&) {}
void hle_VIConfigure(ppc::Ctx& c) {   // r3 = GXRenderModeObj*; viTVmode (first word) >> 2: 0 NTSC, 1 PAL, 2 MPAL, 4 EURGB60
    if (!c.r[3]) return;
    g.tv = ppc::rd32(c, c.r[3]) >> 2;
    g.period = g.tv == 1 ? kViPeriodPal : kViPeriodNtsc;
}
void hle_VIFlush(ppc::Ctx&) {}
void hle_VISetNextFrameBuffer(ppc::Ctx& c) { g.next_fb = c.r[3]; }
void hle_VISetBlack(ppc::Ctx&) {}
void hle_VIGetTvFormat(ppc::Ctx& c) { c.r[3] = g.tv == 1 ? 1 : g.tv == 4 ? 5 : 0; }
void hle_VIGetRetraceCount(ppc::Ctx& c) { c.r[3] = uint32_t(g.retraces.load()); }
void hle_VIWaitForRetrace(ppc::Ctx&) { os_sched().block_on(kViKey); }
void hle_VISetPreRetraceCallback(ppc::Ctx& c) { uint32_t old = g.pre_cb; g.pre_cb = c.r[3]; c.r[3] = old; }
void hle_VISetPostRetraceCallback(ppc::Ctx& c) { uint32_t old = g.post_cb; g.post_cb = c.r[3]; c.r[3] = old; }
```
Add `hle/vi.cpp` to `uc_runtime`. Append to `project/hle_map.txt` (only names that exist in `identified_sdk.txt`/`symbols.txt`): `__VIInit VIInit`, `VIConfigure`, `VIFlush`, `VISetNextFrameBuffer`, `VISetBlack`, `VIGetTvFormat`, `VIWaitForRetrace`; add `VIGetRetraceCount`, `VISetPreRetraceCallback`, `VISetPostRetraceCallback` once Task 9 locates them.

- [ ] **Step 3: Run to verify**

Run: `scripts\dev.bat` — Expected: `test_clock` (with the new schedule checks), `test_vi` and everything else PASSED.

---

### Task 8: Boot driver `uc.exe`, translate script, bring-up script

**Files:**

- Create: `game/uc_main.cpp`, `scripts/translate.bat`, `scripts/bringup.bat`
- Modify: `game/CMakeLists.txt`, `build.bat`, `runtime/dvdfs.h`, `runtime/hle/dvd.cpp`, `runtime/trace.h`, `runtime/trace.cpp`, `.gitignore` (add `build-trace/`, `build-trace.log`)

**Interfaces:**

- Consumes: every runtime piece from Tasks 2–7, the generated `uc_functions` table, `project/identified_sdk.txt`, `project/symbols.txt`.
- Produces:
  - `uc.exe --game <dir> [--seconds N] [--log-dvd]` (default game dir `<install dir>\game`); exit code 0 normal, 3 guest trap or fatal runtime error, 4 no-progress watchdog.
  - `uc::DvdStats { opens, reads, bytes }`, `uc::dvd_stats()`, `uc::dvd_log(bool)`; `uc::trace_dump_all(FILE*, int)`.
  - `scripts\translate.bat` (hle.cfg generation + cached translation, shared by `build.bat` and `bringup.bat`); `scripts\bringup.bat [uc args]` (trace build in `build-trace\`, runs `uc.exe`, prints the log tail).

- [ ] **Step 1: Counters, logging and a registry for trace rings**

`runtime/dvdfs.h`: add to the namespace

```cpp
struct DvdStats { uint64_t opens = 0, reads = 0, bytes = 0; };
DvdStats dvd_stats();
void dvd_log(bool on);   // print "[dvd] open <path>" lines to stderr
```

`runtime/hle/dvd.cpp`: add `DvdStats g_stats; bool g_log = false;` (atomics not needed: only the baton holder touches them), define `dvd_stats()` and `dvd_log()`, increment `g_stats.opens` and log the path in `open_entry` after a successful open (`std::fprintf(stderr, "[dvd] open %s\n", g_fs->path_of(e).c_str());` when `g_log`), and increment `reads`/`bytes` in `hle_DVDReadPrio` and the async completion.

`runtime/trace.h`: add `void trace_dump_all(std::FILE* out, int count = 16);`. Replace `runtime/trace.cpp` with a ring registry so a watchdog can show every thread:

```cpp
#include "trace.h"
#include "ppc_rt.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace {
std::map<uint32_t, std::string> g_syms;
struct Ring { uint32_t a[256]; uint32_t pos = 0; int tid = 0; };
std::mutex g_rings_m;
std::vector<Ring*> g_rings;
Ring& ring() {
    thread_local Ring* r = [] { auto* x = new Ring{}; std::lock_guard<std::mutex> lk(g_rings_m); x->tid = int(g_rings.size()); g_rings.push_back(x); return x; }();
    return *r;
}
void dump_ring(std::FILE* out, const Ring& r, int count) {
    uint32_t n = std::min<uint32_t>(uint32_t(count), std::min<uint32_t>(r.pos, 256));
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t a = r.a[(r.pos - n + i) & 255];
        std::fprintf(out, "  %08X %s\n", a, uc::symbol_name(a).c_str());
    }
    if (r.pos == 0) std::fprintf(out, "  (no trace: build with UC_TRACE=ON)\n");
}
}  // namespace

namespace ppc { void trace_enter(uint32_t a) { Ring& r = ring(); r.a[r.pos++ & 255] = a; } }

namespace uc {
void symbols_load(const std::string& path) {
    std::ifstream f(path);
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string a, n;
        if (ls >> a >> n) g_syms[uint32_t(std::stoul(a, nullptr, 16))] = n;
    }
}
std::string symbol_name(uint32_t a) {
    auto it = g_syms.upper_bound(a);
    if (it == g_syms.begin()) return {};
    --it;
    if (a - it->first > 0x4000) return {};
    if (it->first == a) return it->second;
    char b[24];
    std::snprintf(b, sizeof b, "+0x%X", a - it->first);
    return it->second + b;
}
void trace_dump(std::FILE* out, int count) { dump_ring(out, ring(), count); }
void trace_dump_all(std::FILE* out, int count) {
    std::lock_guard<std::mutex> lk(g_rings_m);
    for (const Ring* r : g_rings) { std::fprintf(out, "host thread %d:\n", r->tid); dump_ring(out, *r, count); }
}
}  // namespace uc
```

- [ ] **Step 2: `scripts\translate.bat` and the `build.bat` change**

`scripts/translate.bat` (expects `ROOT`, `BUILD`, `GEN`, `GAME_DOL` in the environment, the translator built):

```bat
@echo off
rem Generates project\hle.cfg and translates the game when the inputs changed. Returns 1 on failure.
setlocal EnableExtensions
set "CFG=%ROOT%\project\uc.cfg"
set "HLECFG=%ROOT%\project\hle.cfg"
set "TRANSLATOR=%BUILD%\tools\translator\translator.exe"
python -I "%ROOT%\tools\hle_cfg.py" "%ROOT%\project" || (echo ERROR: hle.cfg generation failed & exit /b 1)
set "KEY="
for /f %%h in ('powershell -NoProfile -Command "$h = foreach($p in $env:GAME_DOL,$env:TRANSLATOR,$env:CFG,$env:HLECFG){(Get-FileHash -LiteralPath $p).Hash}; $h -join ''"') do set "KEY=%%h"
if not defined KEY goto :hash_failed
set "OLD="
if exist "%GEN%\.key" set /p OLD=<"%GEN%\.key"
if /I "%KEY%"=="%OLD%" if exist "%GEN%\game_table.cpp" goto :cached
if exist "%GEN%" rmdir /s /q "%GEN%"
"%TRANSLATOR%" --dol "%GAME_DOL%" --cfg "%CFG%" --cfg "%HLECFG%" --out "%GEN%" --shard 150 > "%BUILD%\translate.log" 2>&1
if errorlevel 1 goto :translate_failed
>"%GEN%\.key" echo %KEY%
type "%BUILD%\translate.log"
exit /b 0
:cached
echo generated\ is up to date
exit /b 0
:hash_failed
echo ERROR: could not hash inputs for the translate cache
exit /b 1
:translate_failed
echo ERROR: translation failed
type "%BUILD%\translate.log"
exit /b 1
```

`build.bat`: replace step 3 (from `set "KEY="` through the `:built` label, and the `:hash_failed`/`:translate_failed` labels) by `call "%ROOT%\scripts\translate.bat" || goto :translate_failed` followed by `goto :built`, keeping a single `:translate_failed` label with `echo ERROR: translation failed`.

- [ ] **Step 3: `game/uc_main.cpp`**

```cpp
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
struct FifoSink : uc::MmioDevice {   // GX write-gather pipe: M1 discards the command stream
    uint64_t bytes = 0;
    uint32_t read(uint32_t, int) override { return 0; }
    void write(uint32_t, int size, uint32_t) override { bytes += uint64_t(size); }
};
FifoSink g_fifo;
std::vector<std::unique_ptr<uc::RegFile>> g_regs;

void map_regs(uint32_t base, uint32_t size, const char* name) {
    g_regs.push_back(std::make_unique<uc::RegFile>(size));
    uc::mmio_bus().map(base, size, g_regs.back().get(), name);
}
void map_hardware() {   // physical block offsets of the Wii/GameCube register space seen at 0xCC000000
    map_regs(0xCC000000, 0x80, "CP");     map_regs(0xCC001000, 0x100, "PE");  map_regs(0xCC002000, 0x100, "VI");
    map_regs(0xCC003000, 0x100, "PI");    map_regs(0xCC004000, 0x100, "MI");  map_regs(0xCC005000, 0x200, "DSP");
    map_regs(0xCC006000, 0x40, "DI");     map_regs(0xCC006400, 0x100, "SI");  map_regs(0xCC006800, 0x80, "EXI");
    map_regs(0xCC006C00, 0x20, "AI");     map_regs(0xCD000000, 0x100, "IPC"); map_regs(0xCD800000, 0x1000, "HW");
    uc::mmio_bus().map(0xCC008000, 0x100, &g_fifo, "GX FIFO");
}

void print_stats() {
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

int main(int argc, char** argv) {
    fs::path game = "<install dir>\\game";
    int seconds = 0;
    bool log_dvd = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) game = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (a == "--log-dvd") log_dvd = true;
        else { std::fprintf(stderr, "usage: uc [--game <dir>] [--seconds N] [--log-dvd]\n"); return 2; }
    }
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
    uc::os_hle_init(sched, boot.mem, 0x817F0000);
    sched.irq_ctx().r[1] = 0x817EFF00;
    uc::vi_hle_init(sched, uc::kViPeriodNtsc, nullptr);

    const uint32_t kMainThread = 0x817E0000;   // OSThread slot of the main guest thread (inside the runtime-reserved top of MEM1)
    ppc::wr32(boot, kMainThread + uc::layout::kThreadPriority, 16);
    ppc::wr32(boot, kMainThread + uc::layout::kThreadBasePriority, 16);
    ppc::wr16(boot, kMainThread + uc::layout::kThreadState, 2);   // running
    uint32_t entry = dol.image.entry;
    uc::Thread* mt = sched.create(kMainThread, 16, [entry](ppc::Ctx& c) {
        c.r[1] = 0x817D0000;   // the start code installs its own stack pointer; this one only has to be valid
        uc::call_indirect(c, entry);
    }, &boot);

    if (seconds > 0) {
        std::thread([seconds] { std::this_thread::sleep_for(std::chrono::seconds(seconds)); print_stats(); std::fflush(stderr); ExitProcess(0); }).detach();
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
```

`game/CMakeLists.txt`: inside the `if(EXISTS …)` block add

```cmake
  option(UC_TRACE "record guest function entries (slower; for bring-up)" OFF)
  if(UC_TRACE)
    target_compile_definitions(uc_game PUBLIC UC_TRACE=1)
  endif()
  add_executable(uc uc_main.cpp)
  target_link_libraries(uc PRIVATE uc_game uc_common uc_platform)
  install(TARGETS uc RUNTIME DESTINATION .)
  install(FILES ${CMAKE_SOURCE_DIR}/project/identified_sdk.txt ${CMAKE_SOURCE_DIR}/project/symbols.txt DESTINATION data)
```

`include "dispatch.h"` already provides `uc::call_indirect`.

- [ ] **Step 4: `scripts/bringup.bat`**

```bat
@echo off
rem Trace build of uc.exe in build-trace\ and a run. Usage: scripts\bringup.bat [uc.exe arguments]  (default: --seconds 20)
setlocal EnableExtensions
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "BUILD=%ROOT%\build"
set "GEN=%ROOT%\generated"
if not defined UC_INSTALL_DIR set "UC_INSTALL_DIR=<install dir>"
set "GAME_DOL=%UC_INSTALL_DIR%\game\DATA\sys\main.dol"
set "ARGS=%*"
if not defined ARGS set "ARGS=--seconds 20"
call "%~dp0env.bat" || exit /b 1
if not exist "%BUILD%" mkdir "%BUILD%"
cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl > "%BUILD%\configure.log" 2>&1
if errorlevel 1 goto :fail
cmake --build "%BUILD%" --target translator > "%BUILD%\translator_build.log" 2>&1
if errorlevel 1 goto :fail
call "%~dp0translate.bat" || exit /b 1
if not exist "%ROOT%\build-trace" mkdir "%ROOT%\build-trace"
cmake -S "%ROOT%" -B "%ROOT%\build-trace" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DUC_TRACE=ON > "%ROOT%\build-trace\configure.log" 2>&1
if errorlevel 1 goto :fail
cmake --build "%ROOT%\build-trace" --target uc > "%ROOT%\build-trace\build.log" 2>&1
if errorlevel 1 goto :fail
"%ROOT%\build-trace\game\uc.exe" --game "%UC_INSTALL_DIR%\game" %ARGS% 2> "%ROOT%\build-trace\run.log"
echo uc.exe exit code %errorlevel%
powershell -NoProfile -Command "Get-Content -LiteralPath $env:ROOT'\build-trace\run.log' -Tail 60"
exit /b 0
:fail
echo ERROR: build failed, see the logs in %BUILD% and %ROOT%\build-trace
powershell -NoProfile -Command "Get-Content -LiteralPath $env:ROOT'\build-trace\build.log' -Tail 30 -ErrorAction SilentlyContinue"
exit /b 1
```

- [ ] **Step 5: Verify the pieces without the game, then the first run**

Run: `scripts\dev.bat` — Expected: all tests green; `build\game\uc.exe` exists (it needs `generated\`; run `build.bat --no-install` once).
Run: `build.bat --no-install` — Expected: exit 0, `hle.cfg: N entries`, translation runs once (cache key changed), tests green.
Run: `scripts\bringup.bat --seconds 5` — Expected: the build succeeds and `uc.exe` ends with **one of**: a report starting `guest trap at …` (exit code 3), `fatal: …`, `deadlock: …`, `no progress: …`, or `stats: …` after 5 seconds. Any of these with a readable report is correct for this task; a silent crash or an unreadable report is a bug in Task 2/8 code to fix first.
Record the outcome as iteration 0 in `docs/spikes/R1-bringup-log.md` (create it with the header `# R1 — bring-up log`; one section per iteration: symptom, cause with evidence, change, result).

---

### Task 9: Bring-up loop to milestone M1

**Files:**

- Modify (as needed, one reason each): `project/uc.cfg` (`entry`, `nofunc`, `jumptable`), `project/hle_map.txt`, `runtime/hle/*.cpp`, `runtime/lowmem.cpp`, `game/uc_main.cpp` (`map_hardware`), tests for any new logic
- Update: `docs/spikes/R1-bringup-log.md`

**Interfaces:**

- Consumes: everything from Tasks 1–8.
- Produces: M1 — `uc.exe --seconds 20` exits 0 with `stats: retraces ≥ 1000` and `dvd opens ≥ 1` and no guest trap. The loop below is the procedure; it has no code of its own.

M1 acceptance check (all must hold in one run of `scripts\bringup.bat --seconds 20 --log-dvd`):

1. exit code 0 and the last line of the log is the `stats:` line;
2. `retraces` ≥ 1000 (≈ 59.94 Hz × 20 s);
3. at least one `[dvd] open` line, and the file names show the game loading its own assets (report the first 10);
4. no `guest trap`, `fatal`, `deadlock`, `no progress` line.

- [ ] **Step 1: One iteration = run, classify, make the smallest change, test, log**

1. Run `scripts\bringup.bat --seconds 20 --log-dvd`. Keep `build-trace\run.log`.
2. Classify the first problem in the log and apply **exactly one** change from this table:

| Symptom in the log                                                                                                                      | Cause                                             | Change                                                                                                                                                                                                                                                          |
| --------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `guest trap … unsupported instruction mtsr/mfsr/tlbie/…` or a trap in a function named `__OS…`, `LC…`, `BAT…`, cache or exception setup | privileged hardware setup                         | find the function with the trace (`symbol_name`), confirm with `xref --disasm`, add `<SdkName> Nop` to `project/hle_map.txt` (alias to `hle_Nop`) or `@<addr> Nop` for an unnamed one; if its result is used, `Return0`/`Return1` or a real HLE with a test     |
| `guest trap … indirect call or jump to an address that is not a known function entry`                                                   | missing function entry                            | `xref --disasm` at the caller; if the target is real code add `entry <hex>` to `project/uc.cfg`; if it is data, find why a wrong pointer was loaded (usually an unfilled low-memory global or MMIO value)                                                       |
| `guest trap … illegal or data word`                                                                                                     | execution reached data                            | the previous trace entries show how: a wrong function pointer (see above) or a wrongly split function (`nofunc`/`entry` in `uc.cfg`)                                                                                                                            |
| `mmio: unmapped read/write A size N`                                                                                                    | device missing                                    | map a `RegFile` for the block in `map_hardware()`; if the guest then spins reading a status register (trace shows the same function repeating), add a small `MmioDevice` that returns the value the loop waits for, with a one-line comment naming the register |
| `deadlock:` thread list                                                                                                                 | every thread waits on a key nobody wakes          | the key and `wait_key` show the object: an HLE'd wait whose waker is native or missing (for example a callback the runtime never calls)                                                                                                                         |
| `no progress:` with thread traces                                                                                                       | live-lock in a polling loop                       | the repeating function names the poll: usually a status register or a flag set by a callback or retrace hook                                                                                                                                                    |
| wrong data (garbage names, huge sizes)                                                                                                  | unfilled low-memory global or wrong struct offset | check `R0-layouts.md`; fix the value in `lowmem.cpp` or `os_layout.h`                                                                                                                                                                                           |
| `[OSReport]` text                                                                                                                       | the game explains itself                          | read it first; it often names the missing piece                                                                                                                                                                                                                 |

3. If the change is more than a config line, `Nop` or a `RegFile` (that is, if it contains logic), write the unit test first (RED), then the code (GREEN), then `scripts\dev.bat`.
4. Append to `R1-bringup-log.md`: iteration number, the exact symptom line, the function (with `xref` evidence), the change, and the new first symptom.

- [ ] **Step 2: Stop rules**

Stop the loop and report to the user instead of continuing when any of these holds:

- M1 is reached (go to Task 10);
- 40 iterations were used;
- two consecutive iterations needed a change with no explanation from the code (a guess);
- a translated function appears to compute a wrong result (a guest-code semantics bug: wrong arithmetic, wrong CR/carry, wrong float): capture the function, its inputs and the expected result with `xref --disasm`, add a failing unit test in `test_ppc_*.cpp`, and fix the semantics in `ppc_rt.h` before going on; if the discrepancy cannot be pinned down, **ask the user about Ghidra** (`docs/decisions.md`);
- the same missing information blocks three iterations (for example an unidentified function the game needs): list it for the user.

- [ ] **Step 3: Likely early obstacles (hints, not steps)**

`OSInit` runs translated: expect `Nop`s for `__OSCacheInit`, `LCEnable`, `__OSInitMemoryProtection`, `BATConfig`, `__OSPSInit`, `__OSInitSystemCall`, `OSExceptionInit`, `__OSInitAudioSystem`, `__OSStopAudioSystem`, and possibly `__OSInterruptInit`/`__OSModuleInit`; `AXInit`/`DSP` setup may wait for the DSP mailbox (a `DspStub` device that answers the handshake, or `Nop` for the AX entry points: audio proper is a later plan); `GXInit` and friends write the FIFO and read CP/PE status (`RegFile` first; add `GXDrawDone Nop`, `GXWaitDrawDone Nop` style lines when they spin); `WPADInit`/`KPADInit` need `Nop`/`Return0` stubs and `KPADRead` should report "no data"/"no controller" until the input plan; `OSInitAlarm` etc. run natively on top of the HLE'd alarm calls.

---

### Task 10: Close-out

**Files:**

- Modify: `docs/superpowers/specs/2026-10-12-uc-recomp-design.md` (§6 results/corrections), `docs/decisions.md`, `docs/spikes/R1-bringup-log.md` (summary at the top)

- [ ] **Step 1: Summarise**

At the top of `R1-bringup-log.md` write: iterations used, final `stats:` line, the first ten `[dvd] open` paths, the list of `Nop`/`Return*` stubs added (each with the reason), devices added, struct offsets that were corrected, and what M1 does **not** cover (no GX output, no audio, no movies, no input).

- [ ] **Step 2: Update the spec and decisions**

Spec §6: record the design as built (baton scheduler, event delivery at safe points, `MmioBus`, bottom-up HLE) and any correction found. `docs/decisions.md`: add an entry for GX (call-level versus FIFO-level) with the facts from bring-up (what `GXInit` needs, which GX functions were executed natively), and keep the Ghidra entry current.

- [ ] **Step 3: Hand off**

Report to the user: the M1 numbers, the open items, and a proposal for the next plans in this order: renderer (GX decision, Vulkan skeleton, EFB/XFB, pass structure), input (KPAD/WPAD HLE and mouse), audio (AX/DSP, ADX, DSP-ADPCM), movies (Sofdec), launcher. If M1 was not reached, report the stop rule that fired and the evidence.

---

## Self-review (spec coverage)

- Spec §6 Threads: baton scheduler with priorities, switches only at blocking calls and safe points (Task 4); Time: timebase from the host clock (Tasks 3, 5); DVD from the extracted folder (Task 6); VI/frame pacing with retrace as logic tick (Task 7); Audio, Video, Save: out of scope here (stubs only), listed in the hand-off.
- Spec §5 "no interpreter; unresolved indirect target fatal": unchanged; Task 9 table handles the fatal reports.
- Review Focus 1–7 have tests in Tasks 4, 6, 7; Focus 8 is checked the first time a trap happens on a non-main thread during bring-up (record "not exercised" otherwise).
- Names used across tasks are defined once: `Sched`, `Thread`, `ThreadExit`, `os_hle_init(Sched&, uint8_t*, uint32_t)`, `os_sched()`, `os_dummy_context()`, `format_guest`, `call_guest`, `DvdFs`, `dvd_hle_init`, `dvd_stats`, `dvd_log`, `vi_hle_init`, `vi_stats`, `ViStats::tv_mode`, `next_periodic`, `MmioBus`, `RegFile`, `init_lowmem`, `arena_lo_after_bss`, `trace_dump`, `trace_dump_all`, `symbols_load`, `symbol_name`, `fatal`, `fatal_guest`, `set_exit_hook`, `layout::k*`.
- Out of scope: any picture or sound, real input, movies, the launcher, performance work (register caching), `lwarx` semantics across guest threads beyond one reservation.
