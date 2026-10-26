# Runtime and HLE (boot to the frame loop) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `uc.exe` loads the translated game, boots it through `OSInit`, runs guest threads under an HLE scheduler, serves files from the extracted game folder, ticks the video retrace at 60 Hz and keeps the game's main loop running for 20 seconds without a guest trap. No picture or sound yet (milestone M1).

**Architecture:** The translated SDK code keeps running natively wherever it only manipulates memory. Replaced by HLE (`hle <addr> <Name>` lines in a generated `project/hle.cfg`): the OS scheduler primitives, interrupts and time, DVD file access, the VI retrace wait, and privileged hardware setup functions (no-ops). A baton-passing scheduler runs one guest thread at a time on host threads; timed events (retrace, alarms, DVD completions) are delivered at safe points. Memory-mapped hardware goes through an `MmioBus` of small devices (plain register files by default). Bring-up is a fixed loop: run, read the guest trap or hang, add the smallest HLE function or device behaviour, repeat.

**Tech Stack:** C++20, Win32 threads (`CreateThread` with explicit stack size), CMake/Ninja/clang-cl, Python 3 for helper scripts.

**Spec:** `docs/superpowers/specs/2026-10-12-uc-recomp-design.md` (§6 Runtime). Inputs: `docs/spikes/T1-translation-stats.md`, `docs/spikes/S5-identification.md`, `project/identified_sdk.txt`, `project/hle_required.txt`. Earlier plans: `2026-10-12-foundation-and-spikes.md`, `2026-10-12-analyzer-translator.md` (both done).

## Global Constraints

- No emulator, interpreter, JIT or PowerPC code at run time. A guest address that is not a translated function entry is a fatal error that prints the address.
- Guest threads run one at a time; a switch happens only inside a blocking HLE call, a yield, an exit, or at a safe point where a higher-priority thread became ready. Priority 0 is the highest, 31 the lowest.
- Time base is 60.75 MHz (bus clock 243 MHz / 4); bus clock `0x0E7BE2C0`, CPU clock `0x2B73A840`.
- Guest memory map (from the earlier plan): one 1 GB reservation, host address `base + (ea & 0x3FFFFFFF)`; MEM1 `0x80000000..0x817FFFFF`, MEM2 `0x90000000..0x93FFFFFF`, locked cache `0xE0000000..0xE0003FFF`, MMIO window `0xCC000000..0xCDFFFFFF`. The top 1 MB of MEM1 (`0x81700000..0x817FFFFF`) is reserved for the runtime (interrupt stack, dummy contexts); the arena high mark handed to the game is `0x81700000`.
- Game files: `<game>\DATA\files` of the extracted tree (default `<install dir>\game`). Paths from the guest never escape that root.
- `build.bat` stays one-click, exits non-zero on failure, keeps the paren-safe label style and copies `dist\` to `<install dir>`.
- Project root `<repo>` (spaces in the path; quote everything). The repository now exists (`git`), but commits happen only when the user asks: no commit steps in this plan.
- Test command for every task: `scripts\dev.bat`. Real game: `<install dir>\game\DATA\sys\main.dol`.
- Guest structure layouts (OSThread, DVDFileInfo, low-memory globals) are taken from the translated SDK code and verified in Task 1, never from memory alone.

## Spec deviations and proposals (surface to the user in the hand-off)

1. **Bottom-up HLE:** the spec lists "AX, VI, DVD, OS" as HLE targets. This plan replaces only what cannot run natively (scheduler, interrupts, DVD, VI wait, privileged setup) and lets the rest of `OSInit` etc. run translated. Fewer functions to identify; more MMIO behaviour to supply.
2. **Hardware-level GX is not decided here.** M1 discards FIFO writes; the renderer plan chooses call-level or FIFO-level GX with the 145 named `gx.a` functions in hand.
3. **Audio (AX, ADX), Sofdec movies and real input are out of scope** (stubs only); each gets its own plan.

## Review Focus

1. A guest busy-wait on time or on a flag set by a callback must not live-lock the scheduler (Task 4 test: spinning thread sees an event fire).
2. All guest threads blocked with nothing pending must produce a deadlock report and a non-zero exit, never a silent hang (Task 4 test).
3. Wake-up order is by priority, then FIFO; a woken higher-priority thread preempts the waker at once (Task 4 tests).
4. A blocking call from inside an interrupt-context callback is fatal with a message (Task 4 test).
5. DVD paths: missing file → -1, case-insensitive, leading `/` accepted, `..` rejected, short reads at end of file, zero-length reads (Task 6 tests).
6. A DVD completion posted from the worker thread is delivered while every guest thread is blocked (Task 6 test).
7. Retrace events use an absolute schedule: a late tick must not shift all later ticks (Task 7 test).
8. A trap on a non-main guest thread prints the report with that thread's trace and exits non-zero (Task 8 manual check).

## File Structure

```
common/ppc/format.h, format.cpp      disassembly text for one instruction
tools/xref/main.cpp                  disasm, callers, callees, find-string over the real DOL
tools/hle_cfg.py                     project/hle_map.txt + project/identified_sdk.txt -> project/hle.cfg
project/hle_map.txt                  SDK name -> HLE name (implemented ones only)
project/symbols.txt                  manual names (found by Task 1) merged by the tools
runtime/clock.h, clock.cpp           timebase ticks
runtime/mmio.h, mmio.cpp             MmioDevice, RegFile, MmioBus
runtime/lowmem.h, lowmem.cpp         OS low-memory globals
runtime/sched.h, sched.cpp           Scheduler
runtime/guest_call.h, guest_call.cpp call_guest(): HLE → translated function
runtime/trace.h, trace.cpp           UC_ENTER ring buffer and symbol names
runtime/fatal.h, fatal.cpp           crash report, deadlock report
runtime/hle/*.cpp                    os_thread, os_sync, os_misc, dvd, vi, gx_sync, nop
runtime/platform.cpp                 uc_platform: real ppc::trap / mmio_* / timebase
game/uc_main.cpp                     uc.exe: boot driver
tests/test_clock.cpp, test_mmio.cpp, test_lowmem.cpp, test_sched.cpp, test_os_hle.cpp, test_dvd.cpp, test_vi.cpp, test_format.cpp
docs/spikes/R1-bringup-log.md        bring-up record
scripts/bringup.bat                  trace build + run
```

---

### Task 1: Disassembler, cross-reference tool, missing functions, structure layouts

**Files:**

- Create: `common/ppc/format.h`, `common/ppc/format.cpp`, `tests/test_format.cpp`, `tools/xref/main.cpp`, `tools/xref/CMakeLists.txt`, `project/symbols.txt`, `docs/spikes/R0-layouts.md`
- Modify: `common/CMakeLists.txt`, `tests/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**

- Consumes: `ppc::decode`, `ppc::OpInfo`, `ppc::Kind`, field extractors (`common/ppc/insn.h`), `uc::Image`, `uc::analyze`, `uc::Analysis`, `uc::load_config`.
- Produces: `std::string ppc::format_insn(uint32_t pc, uint32_t word);` and CLI `xref --dol <f> --cfg <f> [--names <f>]... (--disasm <addr> [count] | --callers <name|hex> | --callees <name|hex> | --find-string <text>)`.

- [ ] **Step 1: Write the failing test**

`tests/test_format.cpp`:

```cpp
#include "check.h"
#include "ppc/format.h"

int main() {
    CHECK(ppc::format_insn(0x80003100, 0x38610010) == "addi r3,r1,16");
    CHECK(ppc::format_insn(0x80003100, 0x9421FFF0) == "stwu r1,-16(r1)");
    CHECK(ppc::format_insn(0x80003100, 0x80010014) == "lwz r0,20(r1)");
    CHECK(ppc::format_insn(0x80003100, 0x7C0802A6) == "mflr r0");
    CHECK(ppc::format_insn(0x80003100, 0x7C0803A6) == "mtlr r0");
    CHECK(ppc::format_insn(0x80003100, 0x4E800020) == "blr");
    CHECK(ppc::format_insn(0x80003100, 0x4E800420) == "bctr");
    CHECK(ppc::format_insn(0x80003100, 0x48000009) == "bl 0x80003108");
    CHECK(ppc::format_insn(0x80003100, 0x4BFFFFFC) == "b 0x800030FC");
    CHECK(ppc::format_insn(0x80003100, 0x41820010) == "bc 12,2,0x80003110");
    CHECK(ppc::format_insn(0x80003100, 0x7C642A14) == "add r3,r4,r5");
    CHECK(ppc::format_insn(0x80003100, 0x7C642A15) == "add. r3,r4,r5");
    CHECK(ppc::format_insn(0x80003100, 0x2C030000) == "cmpi cr0,r3,0");
    CHECK(ppc::format_insn(0x80003100, 0x5483103A) == "rlwinm r4,r3,2,0,29");
    CHECK(ppc::format_insn(0x80003100, 0xC0230008) == "lfs f1,8(r3)");
    CHECK(ppc::format_insn(0x80003100, 0x14000000).rfind(".long 0x14000000", 0) == 0);
    return 0;
}
```

Wire `test_format` into the first `foreach` in `tests/CMakeLists.txt`.
Run: `scripts\dev.bat` — Expected: FAIL (`ppc/format.h` not found).

- [ ] **Step 2: Implement `common/ppc/format.{h,cpp}`**

```cpp
// format.h
#pragma once
#include <cstdint>
#include <string>
namespace ppc {
std::string format_insn(uint32_t pc, uint32_t word);
}
```

```cpp
// format.cpp
#include "ppc/format.h"
#include "ppc/insn.h"
#include "ppc/isa.h"
#include <cstdio>

namespace ppc {

static std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%08X", v); return b; }
static std::string r(uint32_t n) { return "r" + std::to_string(n); }
static std::string f(uint32_t n) { return "f" + std::to_string(n); }

std::string format_insn(uint32_t pc, uint32_t w) {
    const OpInfo* op = decode(w);
    if (!op) { char b[32]; std::snprintf(b, sizeof b, ".long 0x%08X", w); return b; }
    std::string name = op->name;
    if (!name.empty() && name.back() == '_') name.back() = '.';
    if (op->rc && rc(w)) name += ".";
    bool fp = (op->primary >= 48 && op->primary <= 55) || (op->primary == 31 && (op->xo == 535 || op->xo == 567 || op->xo == 599 || op->xo == 631 || op->xo == 663 || op->xo == 695 || op->xo == 727 || op->xo == 759 || op->xo == 983));
    auto rt = [&](uint32_t n) { return fp ? f(n) : r(n); };
    std::string o;
    switch (op->kind) {
        case Kind::NONE: return name;
        case Kind::RRR: o = rt(t(w)) + "," + r(a(w)) + "," + r(b(w)); break;
        case Kind::RRS:
            if (op->primary >= 32 && op->primary <= 55) o = rt(t(w)) + "," + std::to_string(simm(w)) + "(" + r(a(w)) + ")";
            else o = r(t(w)) + "," + r(a(w)) + "," + std::to_string(simm(w));
            break;
        case Kind::RRU: o = r(a(w)) + "," + r(t(w)) + "," + std::to_string(uimm(w)); break;
        case Kind::RR: o = r(a(w)) + "," + r(t(w)); break;
        case Kind::AB: o = r(a(w)) + "," + r(b(w)); break;
        case Kind::RB: o = f(t(w)) + "," + f(b(w)); break;
        case Kind::R: o = r(t(w)); break;
        case Kind::CRF: o = "cr" + std::to_string(crfd(w)); break;
        case Kind::RRM: o = r(a(w)) + "," + r(t(w)) + "," + std::to_string(sh(w)) + "," + std::to_string(mb(w)) + "," + std::to_string(me(w)); break;
        case Kind::RRRM: o = r(a(w)) + "," + r(t(w)) + "," + r(b(w)) + "," + std::to_string(mb(w)) + "," + std::to_string(me(w)); break;
        case Kind::CMP: o = "cr" + std::to_string(crfd(w)) + "," + r(a(w)) + "," + r(b(w)); break;
        case Kind::CMPS: o = "cr" + std::to_string(crfd(w)) + "," + r(a(w)) + "," + std::to_string(simm(w)); break;
        case Kind::CMPU: o = "cr" + std::to_string(crfd(w)) + "," + r(a(w)) + "," + std::to_string(uimm(w)); break;
        case Kind::CRB: o = std::to_string(t(w)) + "," + std::to_string(a(w)) + "," + std::to_string(b(w)); break;
        case Kind::MCRF: o = "cr" + std::to_string(crfd(w)) + ",cr" + std::to_string(crfs(w)); break;
        case Kind::SPR:
            if (name == "mfspr" || name == "mtspr") {
                uint32_t s = spr(w);
                const char* nm = s == 8 ? "lr" : s == 9 ? "ctr" : s == 1 ? "xer" : nullptr;
                if (nm) return (name == "mfspr" ? "mf" : "mt") + std::string(nm) + " " + r(t(w));
                o = name == "mfspr" ? r(t(w)) + "," + std::to_string(s) : std::to_string(s) + "," + r(t(w));
            } else o = r(t(w)) + "," + std::to_string(spr(w));
            break;
        case Kind::MTCRF: o = std::to_string(crm(w)) + "," + r(t(w)); break;
        case Kind::RRRR: o = f(t(w)) + "," + f(a(w)) + "," + f(b(w)) + "," + f(c(w)); break;
        case Kind::RAC: o = f(t(w)) + "," + f(a(w)) + "," + f(c(w)); break;
        case Kind::PSQ: o = f(t(w)) + "," + std::to_string(int32_t(w << 20) >> 20) + "(" + r(a(w)) + ")," + std::to_string((w >> 15) & 1) + "," + std::to_string((w >> 12) & 7); break;
        case Kind::PSQX: o = f(t(w)) + "," + r(a(w)) + "," + r(b(w)) + "," + std::to_string((w >> 10) & 1) + "," + std::to_string((w >> 7) & 7); break;
        case Kind::BR: {
            int32_t d = li(w);
            return std::string(lk(w) ? "bl " : "b ") + hex(aa(w) ? uint32_t(d) : pc + uint32_t(d));
        }
        case Kind::BC: {
            int32_t d = bd(w);
            return std::string(lk(w) ? "bcl " : "bc ") + std::to_string(bo(w)) + "," + std::to_string(bi(w)) + "," + hex(aa(w) ? uint32_t(d) : pc + uint32_t(d));
        }
        case Kind::BCLR: return w == 0x4E800020u ? "blr" : (lk(w) ? "bclrl " : "bclr ") + std::to_string(bo(w)) + "," + std::to_string(bi(w));
        case Kind::BCCTR: return w == 0x4E800420u ? "bctr" : w == 0x4E800421u ? "bctrl" : (lk(w) ? "bcctrl " : "bcctr ") + std::to_string(bo(w)) + "," + std::to_string(bi(w));
        case Kind::SC: return "sc";
        default: return name;
    }
    return name + " " + o;
}

}  // namespace ppc
```

Add `ppc/format.cpp` to `uc_common`. Run `scripts\dev.bat` — Expected: `test_format` PASSED. A mismatch in expected text is a formatting bug; fix `format.cpp` (the expected strings follow the standard PowerPC mnemonics).

- [ ] **Step 3: Write `tools/xref`**

`tools/xref/CMakeLists.txt`:

```cmake
add_executable(xref main.cpp)
target_link_libraries(xref PRIVATE uc_common)
```

Root `CMakeLists.txt`: `add_subdirectory(tools/xref)`.

`tools/xref/main.cpp`:

```cpp
#include "analyzer/analyzer.h"
#include "ppc/format.h"
#include "ppc/insn.h"
#include "ppc/isa.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

static std::map<uint32_t, std::string> g_names;

static void load_names(const std::string& path) {   // "<hex> <name> ..." lines; '#' comments
    std::ifstream f(path);
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string a, n;
        if (!(ls >> a >> n)) continue;
        g_names[uint32_t(std::stoul(a, nullptr, 16))] = n;
    }
}
static std::string name_of(uint32_t a) { auto it = g_names.find(a); return it == g_names.end() ? "" : it->second; }

static bool resolve(const std::string& s, uint32_t& out) {
    for (auto& [a, n] : g_names) if (n == s) { out = a; return true; }
    char* end = nullptr;
    unsigned long v = std::strtoul(s.c_str(), &end, 16);
    if (end && !*end && !s.empty()) { out = uint32_t(v); return true; }
    return false;
}

int main(int argc, char** argv) {
    std::string dol_path, cfg_path, cmd, arg;
    int count = 24;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--dol") dol_path = next();
        else if (a == "--cfg") cfg_path = next();
        else if (a == "--names") load_names(next());
        else if (a == "--disasm" || a == "--callers" || a == "--callees" || a == "--find-string") { cmd = a; arg = next(); if (cmd == "--disasm" && i + 1 < argc && argv[i + 1][0] != '-') count = std::atoi(argv[++i]); }
        else { std::fprintf(stderr, "usage: xref --dol <f> [--cfg <f>] [--names <f>]... (--disasm <addr> [n] | --callers <x> | --callees <x> | --find-string <text>)\n"); return 2; }
    }
    if (dol_path.empty() || cmd.empty()) { std::fprintf(stderr, "missing --dol or command\n"); return 2; }
    std::ifstream in(dol_path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", dol_path.c_str()); return 1; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto dol = uc::parse_dol(file);
    if (!dol.ok) { std::fprintf(stderr, "invalid DOL: %s\n", dol.error.c_str()); return 1; }
    uc::Config cfg; std::string err;
    if (!cfg_path.empty() && !uc::load_config(cfg_path, cfg, err)) { std::fprintf(stderr, "config error: %s\n", err.c_str()); return 1; }
    uc::Image img{file, &dol.image};
    uc::Analysis an = uc::analyze(img, cfg);

    auto label = [&](uint32_t a) {
        char b[24]; std::snprintf(b, sizeof b, "%08X", a);
        std::string n = name_of(a);
        if (n.empty()) if (auto* f = an.find_containing(a)) n = name_of(f->entry) + (f->entry == a ? "" : "+" + std::to_string(a - f->entry));
        return std::string(b) + (n.empty() ? "" : " <" + n + ">");
    };

    if (cmd == "--disasm") {
        uint32_t a = 0;
        if (!resolve(arg, a)) { std::fprintf(stderr, "unknown address or name '%s'\n", arg.c_str()); return 1; }
        for (int k = 0; k < count; ++k, a += 4) {
            uint32_t w = 0;
            if (!img.word(a, w)) break;
            std::printf("%s: %08X  %s\n", label(a).c_str(), w, ppc::format_insn(a, w).c_str());
        }
        return 0;
    }
    if (cmd == "--find-string") {   // functions that build the address of a string containing `arg` (lis + addi/ori within 8 insns)
        std::set<uint32_t> hits;
        for (const auto& s : dol.image.sections) {
            std::string text(reinterpret_cast<const char*>(file.data()) + s.file_offset, s.size);
            for (size_t p = text.find(arg); p != std::string::npos; p = text.find(arg, p + 1)) {
                size_t st = p; while (st > 0 && text[st - 1] >= 0x20 && text[st - 1] < 0x7F) --st;
                hits.insert(s.address + uint32_t(st));
            }
        }
        for (uint32_t str : hits) {
            std::printf("string at %08X\n", str);
            for (const auto& f : an.functions)
                for (uint32_t pc = f.entry; pc + 4 < f.end; pc += 4) {
                    uint32_t w = 0; img.word(pc, w);
                    if ((w >> 26) != 15 || ppc::a(w) != 0) continue;
                    for (uint32_t d = 1; d <= 8 && pc + 4 * d < f.end; ++d) {
                        uint32_t w2 = 0; img.word(pc + 4 * d, w2);
                        if ((w2 >> 26) != 14 || ppc::a(w2) != ppc::t(w)) continue;
                        if (((w & 0xFFFF) << 16) + uint32_t(ppc::simm(w2)) == str) { std::printf("  used in %s at %08X\n", label(f.entry).c_str(), pc); break; }
                    }
                }
        }
        return 0;
    }
    uint32_t target = 0;
    if (!resolve(arg, target)) { std::fprintf(stderr, "unknown address or name '%s'\n", arg.c_str()); return 1; }
    const uc::Function* tf = an.find_containing(target);
    if (cmd == "--callees") {
        if (!tf) { std::fprintf(stderr, "no function at %08X\n", target); return 1; }
        std::set<uint32_t> seen;
        for (uint32_t pc = tf->entry; pc < tf->end; pc += 4) {
            uint32_t w = 0; img.word(pc, w);
            if ((w >> 26) == 18 && (w & 3) == 1) { uint32_t t = pc + uint32_t(ppc::li(w)); if (seen.insert(t).second) std::printf("%s\n", label(t).c_str()); }
        }
        return 0;
    }
    for (const auto& f : an.functions) {   // --callers: bl/b to `target`, plus data words holding its address
        for (uint32_t pc = f.entry; pc < f.end; pc += 4) {
            uint32_t w = 0; img.word(pc, w);
            if ((w >> 26) == 18 && !(w & 2) && pc + uint32_t(ppc::li(w)) == target) std::printf("%s  at %08X (%s)\n", label(f.entry).c_str(), pc, (w & 1) ? "bl" : "b");
        }
    }
    for (const auto& s : dol.image.sections) {
        if (s.is_text) continue;
        for (uint32_t a = s.address; a + 4 <= s.address + s.size; a += 4) { uint32_t v = 0; img.word(a, v); if (v == target) std::printf("data pointer at %08X\n", a); }
    }
    return 0;
}
```

Run: `scripts\dev.bat` — Expected: builds; `build\tools\xref\xref.exe --dol <main.dol> --cfg project\uc.cfg --names project\identified_sdk.txt --disasm 8025FC94 8` prints eight lines starting with `8025FC94 <OSInitMutex>`.

- [ ] **Step 4: Locate the missing functions and write `project/symbols.txt`**

Names to find (from `docs/spikes/S5-identification.md`): `OSInit`, `OSReport`, `OSPanic`, `OSGetTick`, `OSJoinThread`, `DVDClose`, `WPADInit`, `WPADRead`, `KPADInit`, `KPADRead`, `KPADSetPosParam`, plus the two unattributed privileged functions `802A26F4` and `802A3EB0`. Use these commands (set `X=build\tools\xref\xref.exe --dol "<install dir>\game\DATA\sys\main.dol" --cfg project\uc.cfg --names project\identified_sdk.txt`):

| Target                                    | Procedure                                                                                                                      | Expected evidence                                                                                                                    |
| ----------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------ |
| `OSPanic`                                 | `%X% --find-string "Panic:"`, then `--callers` of the user function                                                            | a small function that formats then calls a halting loop; callers pass `__FILE__`, line, message                                      |
| `OSReport`                                | callees of `OSPanic`, pick the one called before the halt                                                                      | variadic function (saves `f1`–`f8` with `stfd` under `crclr`/`creqv 6,6,6` check)                                                    |
| `OSInit`                                  | `--callers __OSInitAlarm`, `--callers __OSInterruptInit`                                                                       | one function calls both plus `__OSInitSram`/`__OSContextInit`; it is called once from the startup code reached from entry `8000403C` |
| `OSGetTick`                               | `--disasm` of small functions near `OSGetTime` (named); body is `mftb rD,268; blr`                                             | 2–3 instructions                                                                                                                     |
| `OSJoinThread`                            | neighbours of `OSCreateThread`/`OSExitThread` in `os.a` order                                                                  | calls `OSSleepThread` on the target's join queue                                                                                     |
| `DVDClose`                                | neighbours of `DVDOpen`                                                                                                        | calls `DVDCancel`-like function, returns 1                                                                                           |
| `WPADInit`                                | `--find-string "WPADInit()"`                                                                                                   | function using that string with `OSReport`-style call                                                                                |
| `WPADRead`                                | callers of the named `WPADiHIDParser`/`WPADProbe`                                                                              | takes `(chan, WPADStatus*)`                                                                                                          |
| `KPADInit`, `KPADRead`, `KPADSetPosParam` | callers of the named `WPAD*` functions; the KPAD banner is at `0x80432BD0` (data), its functions live in the neighbouring text | `KPADRead(chan, KPADStatus*, count)` calls `WPADRead` and filters the pointer                                                        |
| `802A26F4`, `802A3EB0`                    | `--callers`, `--callees`                                                                                                       | TRK / OS support code                                                                                                                |

Write what was found, one per line, to `project/symbols.txt` as `<addr> <name> manual <evidence>` (same format as `identified_sdk.txt`, which tools accept via repeated `--names`). A name that cannot be located after the evidence above goes in the file as a comment line `# NOT FOUND <name>: <what was tried>`; do not guess.
**Gate:** `OSInit`, `OSReport`, `OSPanic`, `KPADRead` must be located. If `KPADRead` or `WPADRead` is not found, stop and put the Ghidra question to the user (see `docs/decisions.md`).

- [ ] **Step 5: Verify structure layouts and write `docs/spikes/R0-layouts.md`**

For each structure, disassemble the translated SDK function that writes it and record the offsets actually used:

| Structure                              | Function to read (`--disasm`)                     | Fields to record                                                                                                                                                     |
| -------------------------------------- | ------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `OSThread`                             | `OSCreateThread` (named), `__OSThreadInit`        | offsets of state (u16), attr (u16), suspend count, priority, base priority, join queue, stack base, stack end, error, `specific[]`; the size of the embedded context |
| Low memory                             | list from the scan below                          | meaning of each offset                                                                                                                                               |
| `DVDFileInfo` / command block          | `DVDFastOpen`, `DVDReadAsyncPrio`                 | offsets of state, start address, length, callback                                                                                                                    |
| `OSAlarm`, `OSMutex`, `OSMessageQueue` | `OSSetAlarm`, `OSInitMutex`, `OSInitMessageQueue` | sizes and which fields the game could read directly                                                                                                                  |

Low-memory offsets the game reads or writes directly (scan of `main.dol`, count of sites): `0x00` game id (5r/3w), `0x04`, `0x06`, `0x08`, `0x38`, `0x60`, `0xC4`, `0xCC` (9r), `0xD4`, `0xD8`, `0xDC`, `0xE0`, `0xE4` (current thread, 7r), `0xF4`, **`0xF8` bus clock (75 sites)**, and `0x30C4…0x3184` (OS globals: memory sizes, arena marks, IOS/version info). For each, write one line "offset, meaning, value to provide" using the reader at the example address printed by the scan (`--disasm <reader address-8> 16`). Values that cannot be derived yet stay "0 until a bring-up trap shows otherwise".
Expected: `R0-layouts.md` states the OSThread offsets with the disassembly line that proves each, and a value table for low memory; Task 3 and Task 5 consume it.

---

### Task 2: HLE plumbing — config merge, `hle.cfg` generator, trace, platform, fatal reports

**Files:**

- Create: `project/hle_map.txt`, `tools/hle_cfg.py`, `runtime/trace.h`, `runtime/trace.cpp`, `runtime/fatal.h`, `runtime/fatal.cpp`, `runtime/platform.cpp`, `runtime/hle/nop.cpp`, `runtime/guest_call.h`, `runtime/guest_call.cpp`
- Modify: `tools/translator/main.cpp` (repeatable `--cfg`), `common/translator/emit.cpp` (`UC_ENTER`), `tests/test_emit.cpp`, `runtime/ppc_rt.h`, `runtime/CMakeLists.txt`, `game/CMakeLists.txt`, `build.bat`

**Interfaces:**

- Consumes: `uc::Config`, `uc::load_config`, `uc::emit_function`, `ppc::Ctx`, `uc::call_indirect`.
- Produces:
  - `UC_ENTER(addr)`: macro in `ppc_rt.h`; no-op unless `UC_TRACE` is defined, then calls `ppc::trace_enter(uint32_t)`.
  - `namespace uc`: `void trace_dump(std::FILE*, int count = 64);`, `void symbols_load(const std::string& path);`, `std::string symbol_name(uint32_t addr);`
  - `[[noreturn]] void uc::fatal(const char* fmt, ...);`, `[[noreturn]] void uc::fatal_guest(uint32_t addr, const char* what);`
  - `uint32_t uc::call_guest(ppc::Ctx& c, uint32_t addr, std::initializer_list<uint32_t> args = {});` (sets `r3…`, calls, returns `r3`).
  - HLE functions `hle_Nop`, `hle_Return0`, `hle_Return1` (all `void(ppc::Ctx&)`).
  - CLI: `translator --cfg a --cfg b` merges configs in order.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_emit.cpp` (inside `main`, before `return 0;`):

```cpp
    {   // every translated function starts with the trace hook (a no-op macro unless UC_TRACE is defined)
        std::string t = uc::emit_function(p, f, st);
        CHECK(t.find("UC_ENTER(0x80003100u);") != std::string::npos);
        CHECK(t.find("UC_ENTER") < t.find("L_80003100:;") || t.find("L_80003100:;") == std::string::npos);
    }
```

Add `tests/test_cfg_merge.cmake` (run through ctest) — checks the CLI accepts two `--cfg` files:

```cmake
file(WRITE ${OUT}/a.cfg "entry 80003100\n")
file(WRITE ${OUT}/b.cfg "nofunc 80003200\n")
execute_process(COMMAND ${TRANSLATOR} --dol ${DOL} --cfg ${OUT}/a.cfg --cfg ${OUT}/b.cfg --out ${OUT}/gen RESULT_VARIABLE r)
if(r)
  message(FATAL_ERROR "translator rejected two --cfg arguments (${r})")
endif()
execute_process(COMMAND ${TRANSLATOR} --dol ${DOL} --cfg ${OUT}/missing.cfg --out ${OUT}/gen2 RESULT_VARIABLE r2)
if(NOT r2)
  message(FATAL_ERROR "translator accepted a missing --cfg file")
endif()
```

In `tests/e2e/CMakeLists.txt` append:

```cmake
add_test(NAME translator_two_cfgs
  COMMAND ${CMAKE_COMMAND} -DTRANSLATOR=$<TARGET_FILE:translator> -DDOL=${CMAKE_CURRENT_BINARY_DIR}/gen_jt1/jt1.dol
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}/cfgs -P ${CMAKE_CURRENT_SOURCE_DIR}/../test_cfg_merge.cmake)
```

Run: `scripts\dev.bat` — Expected: FAIL (`UC_ENTER` missing; `translator_two_cfgs` fails because the second `--cfg` overwrites or is rejected).

- [ ] **Step 2: Implement**

`runtime/ppc_rt.h` — add right after the `PPC_INLINE` define:

```cpp
#ifdef UC_TRACE
namespace ppc { void trace_enter(uint32_t addr); }
#define UC_ENTER(a) ppc::trace_enter(a)
#else
#define UC_ENTER(a) ((void)0)
#endif
```

`common/translator/emit.cpp`, in `emit_function`: after the line that opens the function/body (`o << "void " << fn_name… {\n"` and the `static void body_…` variant) emit the hook as the first statement, and for the secondary body variant put it after the `switch` line is wrong because the `goto` jumps over it, so emit it before the `switch`:

```cpp
    if (has_sec) {
        o << "static void body_" << hex8(f.entry) << "(ppc::Ctx& c, uint32_t at) {\n  UC_ENTER(0x" << hex8(f.entry) << "u);\n  switch (at) { ";
        // ...unchanged
    } else {
        o << "void " << fn_name(f.entry) << "(ppc::Ctx& c) {\n  UC_ENTER(0x" << hex8(f.entry) << "u);\n";
    }
```

`tools/translator/main.cpp`: replace the single `cfg_path` with `std::vector<std::string> cfg_paths;` (`--cfg` appends) and load each into the same `p.cfg`; a missing file is an error (exit 1) as before.

`runtime/trace.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace uc {
void trace_dump(std::FILE* out, int count = 64);   // last guest functions entered by the calling thread (needs UC_TRACE)
void symbols_load(const std::string& path);        // "<hex> <name> ..." lines; '#' comments
std::string symbol_name(uint32_t addr);            // "name", "name+0x1C" or ""
}
```

`runtime/trace.cpp`:

```cpp
#include "trace.h"
#include "ppc_rt.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

namespace {
std::map<uint32_t, std::string> g_syms;
thread_local uint32_t t_ring[256];
thread_local uint32_t t_pos;
}

namespace ppc { void trace_enter(uint32_t a) { t_ring[t_pos++ & 255] = a; } }

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
    char b[24];
    if (it->first == a) return it->second;
    std::snprintf(b, sizeof b, "+0x%X", a - it->first);
    return it->second + b;
}
void trace_dump(std::FILE* out, int count) {
    uint32_t n = std::min<uint32_t>(uint32_t(count), std::min<uint32_t>(t_pos, 256));
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t a = t_ring[(t_pos - n + i) & 255];
        std::fprintf(out, "  %08X %s\n", a, symbol_name(a).c_str());
    }
    if (t_pos == 0) std::fprintf(out, "  (no trace: build with UC_TRACE=ON)\n");
}
}  // namespace uc
```

`runtime/fatal.h`:

```cpp
#pragma once
#include <cstdint>
namespace uc {
[[noreturn]] void fatal(const char* fmt, ...);
[[noreturn]] void fatal_guest(uint32_t addr, const char* what);
void set_exit_hook(void (*fn)());   // called once before the process exits (stats, flush)
}
```

`runtime/fatal.cpp`:

```cpp
#include "fatal.h"
#include "trace.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace uc {
static void (*g_hook)();
void set_exit_hook(void (*fn)()) { g_hook = fn; }

static void die(int code) {
    if (g_hook) g_hook();
    std::fflush(stderr);
    ExitProcess(UINT(code));   // kills the other host threads too
}
void fatal(const char* fmt, ...) {
    std::va_list ap; va_start(ap, fmt);
    std::fprintf(stderr, "fatal: ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
    trace_dump(stderr);
    die(3);
}
void fatal_guest(uint32_t addr, const char* what) {
    std::fprintf(stderr, "guest trap at %08X: %s", addr, what);
    std::string s = symbol_name(addr);
    if (!s.empty()) std::fprintf(stderr, "  [%s]", s.c_str());
    std::fprintf(stderr, "\nlast guest functions on this thread:\n");
    trace_dump(stderr);
    die(3);
}
}  // namespace uc
```

`runtime/guest_call.h/.cpp`:

```cpp
// guest_call.h
#pragma once
#include "ppc_rt.h"
#include <initializer_list>
namespace uc {
uint32_t call_guest(ppc::Ctx& c, uint32_t addr, std::initializer_list<uint32_t> args = {});
}
// guest_call.cpp
#include "guest_call.h"
#include "dispatch.h"
namespace uc {
uint32_t call_guest(ppc::Ctx& c, uint32_t addr, std::initializer_list<uint32_t> args) {
    int i = 3;
    for (uint32_t a : args) c.r[i++] = a;
    uint32_t saved_lr = c.lr;
    call_indirect(c, addr);
    c.lr = saved_lr;
    return c.r[3];
}
}
```

`runtime/platform.cpp` (real hook implementations for `uc.exe`; tests keep using `tests/mmio_stub.cpp`):

```cpp
#include "clock.h"
#include "fatal.h"
#include "mmio.h"
#include "ppc_rt.h"

namespace ppc {
uint32_t mmio_read(uint32_t ea, int size) { return uc::mmio_bus().read(ea, size); }
void mmio_write(uint32_t ea, int size, uint32_t v) { uc::mmio_bus().write(ea, size, v); }
[[noreturn]] void trap(uint32_t addr, const char* what) { uc::fatal_guest(addr, what); }
uint64_t timebase() { return uc::now_ticks(); }
}  // namespace ppc
```

(`clock.h` and `mmio.h` come from Task 3; add `platform.cpp` to the `uc_platform` target in that task so this task still builds.)
`runtime/hle/nop.cpp`:

```cpp
#include "ppc_rt.h"
void hle_Nop(ppc::Ctx&) {}
void hle_Return0(ppc::Ctx& c) { c.r[3] = 0; }
void hle_Return1(ppc::Ctx& c) { c.r[3] = 1; }
```

`runtime/CMakeLists.txt`: `add_library(uc_runtime STATIC guest_mem.cpp dispatch.cpp trace.cpp fatal.cpp guest_call.cpp hle/nop.cpp)`.

- [ ] **Step 3: `tools/hle_cfg.py` and `project/hle_map.txt`**

`project/hle_map.txt` format, one per line: `<SdkName> [HleName]` (HleName defaults to SdkName), or `@<hex address> <HleName>` for a function found by hand, `#` comments. Start with only functions whose HLE exists (this task: none; later tasks append lines):

```
# SDK function replaced by hle_<HleName>. Only implemented HLE functions belong here (a missing one is a link error).
```

`tools/hle_cfg.py`:

```python
# Generates project/hle.cfg from project/hle_map.txt + project/identified_sdk.txt (+ project/symbols.txt).
# Usage: python -I tools/hle_cfg.py <project dir>
import re
import sys
from pathlib import Path

proj = Path(sys.argv[1])
names = {}  # sdk name -> [addresses]
for fn in ("identified_sdk.txt", "symbols.txt"):
    p = proj / fn
    if not p.exists():
        continue
    for line in p.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 2:
            names.setdefault(parts[1], []).append(int(parts[0], 16))

out, errors = [], []
for raw in (proj / "hle_map.txt").read_text(encoding="utf-8").splitlines():
    line = raw.split("#", 1)[0].strip()
    if not line:
        continue
    parts = line.split()
    if parts[0].startswith("@"):
        out.append("hle %08X %s" % (int(parts[0][1:], 16), parts[1]))
        continue
    hle = parts[1] if len(parts) > 1 else parts[0]
    if not re.fullmatch(r"[A-Za-z0-9_]+", hle):
        errors.append("bad HLE name %r" % hle)
        continue
    addrs = sorted(set(names.get(parts[0], [])))
    if not addrs:
        errors.append("SDK name %r not found in identified_sdk.txt / symbols.txt" % parts[0])
        continue
    for a in addrs:
        out.append("hle %08X %s  # %s" % (a, hle, parts[0]))
if errors:
    sys.stderr.write("\n".join(errors) + "\n")
    sys.exit(1)
(proj / "hle.cfg").write_text("# generated by tools/hle_cfg.py - do not edit\n" + "\n".join(out) + "\n", encoding="utf-8")
print("hle.cfg: %d entries" % len(out))
```

Add `project/hle.cfg` to `.gitignore`? It is derived; keep it out of the repository: add the line `project/hle.cfg`.
`build.bat`: before step 3 run `python -I "%ROOT%\tools\hle_cfg.py" "%ROOT%\project" || goto :hle_failed`, add `:hle_failed` label (`echo ERROR: hle.cfg generation failed`), pass `--cfg "%CFG%" --cfg "%ROOT%\project\hle.cfg"` to the translator, and include `project\hle.cfg` in the hash list (`$env:CFG,$env:HLECFG`; set `HLECFG`).

- [ ] **Step 4: Run to verify**

Run: `scripts\dev.bat` — Expected: all earlier tests plus `translator_two_cfgs` and the `UC_ENTER` checks PASSED. Run `build.bat --no-install` — Expected: exit 0 and `hle.cfg: 0 entries`.

---

### Task 3: Clock, MMIO bus, low memory (TDD)

**Files:**

- Create: `runtime/clock.h`, `runtime/clock.cpp`, `runtime/mmio.h`, `runtime/mmio.cpp`, `runtime/lowmem.h`, `runtime/lowmem.cpp`, `tests/test_clock.cpp`, `tests/test_mmio.cpp`, `tests/test_lowmem.cpp`
- Modify: `runtime/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**

- Produces:
  - `namespace uc`: `constexpr uint64_t kTimebaseHz = 60750000;` `uint64_t now_ticks();` `uint64_t ms_to_ticks(uint64_t);` `uint64_t ticks_to_ms(uint64_t);`
  - `struct MmioDevice { virtual uint32_t read(uint32_t off, int size) = 0; virtual void write(uint32_t off, int size, uint32_t v) = 0; virtual ~MmioDevice() = default; };`
  - `struct RegFile : MmioDevice { explicit RegFile(uint32_t size); }` (big-endian, reads return the last write).
  - `class MmioBus { void map(uint32_t base, uint32_t size, MmioDevice*, std::string name); uint32_t read(uint32_t ea, int size); void write(uint32_t ea, int size, uint32_t v); size_t unmapped_count() const; };` `MmioBus& mmio_bus();`
  - `struct LowMemInfo { uint32_t mem1_size = 0x01800000, mem2_size = 0x04000000, arena_lo = 0, arena_hi = 0x81700000, tv_format = 0; };` `void init_lowmem(ppc::Ctx& c, const uint8_t* disc_header32, const LowMemInfo&);` `uint32_t arena_lo_after_bss(uint32_t bss_addr, uint32_t bss_size);`

- [ ] **Step 1: Write the failing tests**

`tests/test_clock.cpp`:

```cpp
#include "check.h"
#include "clock.h"
#include <thread>

int main() {
    CHECK(uc::ms_to_ticks(1000) == uc::kTimebaseHz);
    CHECK(uc::ticks_to_ms(uc::kTimebaseHz / 2) == 500);
    uint64_t a = uc::now_ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    uint64_t b = uc::now_ticks();
    CHECK(b > a);
    uint64_t ms = uc::ticks_to_ms(b - a);
    CHECK(ms >= 25 && ms < 500);   // monotonic and scaled to the 60.75 MHz time base
    return 0;
}
```

`tests/test_mmio.cpp`:

```cpp
#include "check.h"
#include "mmio.h"

struct Probe : uc::MmioDevice {
    uint32_t last_off = 0xFFFFFFFF, last_val = 0; int last_size = 0;
    uint32_t read(uint32_t off, int size) override { last_off = off; last_size = size; return 0x11223344u >> (32 - 8 * size); }
    void write(uint32_t off, int size, uint32_t v) override { last_off = off; last_size = size; last_val = v; }
};

int main() {
    uc::MmioBus bus;
    Probe p; uc::RegFile regs(0x100);
    bus.map(0xCC002000, 0x100, &regs, "VI");
    bus.map(0xCC008000, 0x4, &p, "FIFO");
    // register file: big-endian, read-back, mixed widths
    bus.write(0xCC002010, 4, 0xAABBCCDD);
    CHECK(bus.read(0xCC002010, 4) == 0xAABBCCDD);
    CHECK(bus.read(0xCC002010, 2) == 0xAABB && bus.read(0xCC002012, 2) == 0xCCDD);
    CHECK(bus.read(0xCC002013, 1) == 0xDD);
    bus.write(0xCC002011, 1, 0x55);
    CHECK(bus.read(0xCC002010, 4) == 0xAA55CCDD);
    // routing and offsets
    bus.write(0xCC008000, 4, 0x1234);
    CHECK(p.last_off == 0 && p.last_val == 0x1234 && p.last_size == 4);
    CHECK(bus.read(0xCC008000, 2) == 0x1122);
    // boundaries: first byte past a range is unmapped
    CHECK(bus.unmapped_count() == 0);
    CHECK(bus.read(0xCC002100, 4) == 0);
    CHECK(bus.read(0xCC001FFC, 4) == 0);
    CHECK(bus.unmapped_count() == 2);
    bus.write(0xCC0FFFFC, 4, 1);   // unmapped write is counted, not fatal
    CHECK(bus.unmapped_count() == 3);
    // overlap is rejected
    bool threw = false;
    try { bus.map(0xCC0020F0, 0x20, &p, "overlap"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    return 0;
}
```

`tests/test_lowmem.cpp`:

```cpp
#include "check.h"
#include "lowmem.h"
#include "ppc_fixture.h"
using namespace ppc;

int main() {
    Fix f; Ctx& c = f.c;
    uint8_t hdr[32] = {'R', 'B', 'U', 'E', '0', '8', 0, 0};
    uc::LowMemInfo info;
    info.arena_lo = uc::arena_lo_after_bss(0x8043A680, 0x002910FC);
    CHECK(info.arena_lo == 0x806CB780);            // end of BSS 0x806CB77C rounded up to 32 bytes
    uc::init_lowmem(c, hdr, info);
    CHECK(rd32(c, 0x80000000) == 0x52425545);       // "RBUE" copied from the disc header
    CHECK(rd16(c, 0x80000004) == 0x3038);           // "08"
    CHECK(rd32(c, 0x80000020) == 0x0D15EA5E);
    CHECK(rd32(c, 0x80000028) == 0x01800000);       // MEM1 size
    CHECK(rd32(c, 0x80000030) == info.arena_lo);
    CHECK(rd32(c, 0x80000034) == 0x81700000);
    CHECK(rd32(c, 0x800000F8) == 243000000);        // bus clock
    CHECK(rd32(c, 0x800000FC) == 729000000);        // CPU clock
    CHECK(rd32(c, 0x800000CC) == 0);                // NTSC
    CHECK(rd32(c, 0x80003118) == 0x04000000);       // MEM2 physical size
    return 0;
}
```

Wire `test_clock test_mmio test_lowmem` into `tests/CMakeLists.txt` in a new loop that links `test_support` (and `uc_runtime`) like the `test_ppc_*` loop (add them to that loop's list). Run: `scripts\dev.bat` — Expected: FAIL (headers not found).

- [ ] **Step 2: Implement**

`runtime/clock.h`:

```cpp
#pragma once
#include <cstdint>
namespace uc {
constexpr uint64_t kTimebaseHz = 60750000ull;   // bus clock 243 MHz / 4
uint64_t now_ticks();
uint64_t ms_to_ticks(uint64_t ms);
uint64_t ticks_to_ms(uint64_t ticks);
}
```

`runtime/clock.cpp`:

```cpp
#include "clock.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace uc {
uint64_t now_ticks() {
    static const uint64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    uint64_t c = uint64_t(q.QuadPart);
    return (c / freq) * kTimebaseHz + (c % freq) * kTimebaseHz / freq;   // (c % freq) * hz < 2^64 for any real counter frequency
}
uint64_t ms_to_ticks(uint64_t ms) { return ms * (kTimebaseHz / 1000); }
uint64_t ticks_to_ms(uint64_t t) { return t / (kTimebaseHz / 1000); }
}
```

`runtime/mmio.h`:

```cpp
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace uc {

struct MmioDevice {
    virtual ~MmioDevice() = default;
    virtual uint32_t read(uint32_t off, int size) = 0;
    virtual void write(uint32_t off, int size, uint32_t v) = 0;
};

// Plain registers: big-endian bytes, reads return the last write.
struct RegFile : MmioDevice {
    explicit RegFile(uint32_t size) : bytes(size) {}
    uint32_t read(uint32_t off, int size) override;
    void write(uint32_t off, int size, uint32_t v) override;
    std::vector<uint8_t> bytes;
};

class MmioBus {
public:
    void map(uint32_t base, uint32_t size, MmioDevice* dev, std::string name);   // throws std::invalid_argument on overlap
    uint32_t read(uint32_t ea, int size);
    void write(uint32_t ea, int size, uint32_t v);
    size_t unmapped_count() const { return unmapped_; }
private:
    struct Range { uint32_t base, size; MmioDevice* dev; std::string name; };
    const Range* find(uint32_t ea) const;
    void report(const char* what, uint32_t ea, int size);
    std::vector<Range> ranges_;   // sorted by base
    std::set<std::pair<uint32_t, int>> seen_;
    size_t unmapped_ = 0;
};

MmioBus& mmio_bus();
}  // namespace uc
```

`runtime/mmio.cpp`:

```cpp
#include "mmio.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace uc {

uint32_t RegFile::read(uint32_t off, int size) {
    if (off + size > bytes.size()) return 0;
    uint32_t v = 0;
    for (int i = 0; i < size; ++i) v = v << 8 | bytes[off + i];
    return v;
}
void RegFile::write(uint32_t off, int size, uint32_t v) {
    if (off + size > bytes.size()) return;
    for (int i = size - 1; i >= 0; --i, v >>= 8) bytes[off + i] = uint8_t(v);
}

void MmioBus::map(uint32_t base, uint32_t size, MmioDevice* dev, std::string name) {
    for (const auto& r : ranges_)
        if (base < r.base + r.size && r.base < base + size) throw std::invalid_argument("MMIO range overlaps " + r.name);
    ranges_.push_back({base, size, dev, std::move(name)});
    std::sort(ranges_.begin(), ranges_.end(), [](const Range& a, const Range& b) { return a.base < b.base; });
}

const MmioBus::Range* MmioBus::find(uint32_t ea) const {
    auto it = std::upper_bound(ranges_.begin(), ranges_.end(), ea, [](uint32_t v, const Range& r) { return v < r.base; });
    if (it == ranges_.begin()) return nullptr;
    --it;
    return ea - it->base < it->size ? &*it : nullptr;
}

void MmioBus::report(const char* what, uint32_t ea, int size) {
    ++unmapped_;
    if (seen_.size() < 64 && seen_.insert({ea, size}).second) std::fprintf(stderr, "mmio: unmapped %s %08X size %d\n", what, ea, size);
}

uint32_t MmioBus::read(uint32_t ea, int size) {
    if (const Range* r = find(ea)) return r->dev->read(ea - r->base, size);
    report("read", ea, size);
    return 0;
}
void MmioBus::write(uint32_t ea, int size, uint32_t v) {
    if (const Range* r = find(ea)) { r->dev->write(ea - r->base, size, v); return; }
    report("write", ea, size);
}

MmioBus& mmio_bus() { static MmioBus b; return b; }
}  // namespace uc
```

In the test, `Probe::read` returns `0x11223344u >> (32 - 8*size)`: size 2 → `0x1122`. ✓.

`runtime/lowmem.h`:

```cpp
#pragma once
#include "ppc_rt.h"
namespace uc {
struct LowMemInfo {
    uint32_t mem1_size = 0x01800000, mem2_size = 0x04000000;
    uint32_t arena_lo = 0, arena_hi = 0x81700000;
    uint32_t tv_format = 0;   // 0 NTSC, 1 PAL
};
uint32_t arena_lo_after_bss(uint32_t bss_addr, uint32_t bss_size);   // end of BSS rounded up to 32 bytes
void init_lowmem(ppc::Ctx& c, const uint8_t* disc_header32, const LowMemInfo& info);
}
```

`runtime/lowmem.cpp` (values for offsets not proven yet are marked and re-verified by `docs/spikes/R0-layouts.md`):

```cpp
#include "lowmem.h"

namespace uc {

uint32_t arena_lo_after_bss(uint32_t bss_addr, uint32_t bss_size) { return (bss_addr + bss_size + 31u) & ~31u; }

void init_lowmem(ppc::Ctx& c, const uint8_t* hdr, const LowMemInfo& i) {
    for (uint32_t k = 0; k < 32; ++k) ppc::wr8(c, 0x80000000u + k, hdr[k]);   // disc header: game id, maker, version
    ppc::wr32(c, 0x80000020, 0x0D15EA5E);      // boot magic
    ppc::wr32(c, 0x80000024, 1);               // version
    ppc::wr32(c, 0x80000028, i.mem1_size);
    ppc::wr32(c, 0x80000030, i.arena_lo);
    ppc::wr32(c, 0x80000034, i.arena_hi);
    ppc::wr32(c, 0x800000CC, i.tv_format);
    ppc::wr32(c, 0x800000F8, 243000000);       // bus clock
    ppc::wr32(c, 0x800000FC, 729000000);       // CPU clock
    ppc::wr32(c, 0x80003100, i.mem1_size);     // physical MEM1 size       (verify: R0-layouts.md)
    ppc::wr32(c, 0x80003104, i.mem1_size);     // simulated MEM1 size
    ppc::wr32(c, 0x80003118, i.mem2_size);     // physical MEM2 size
    ppc::wr32(c, 0x8000311C, i.mem2_size);     // simulated MEM2 size
    // Remaining 0x30xx globals and 0xC4..0xF4 start at 0 and are filled in only when a bring-up trap shows a reader needs them.
}

}  // namespace uc
```

`runtime/CMakeLists.txt`: add `clock.cpp mmio.cpp lowmem.cpp` to `uc_runtime`; add the `uc_platform` target:

```cmake
add_library(uc_platform STATIC platform.cpp)
target_link_libraries(uc_platform PUBLIC uc_runtime)
```

Run: `scripts\dev.bat` — Expected: `test_clock`, `test_mmio`, `test_lowmem` and everything earlier PASSED.
