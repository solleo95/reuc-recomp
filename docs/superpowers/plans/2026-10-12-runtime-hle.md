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
- Game files: `<game>\DATA\files` of the extracted tree (default `S:\Games\Umbrella Chronicles\game`). Paths from the guest never escape that root.
- `build.bat` stays one-click, exits non-zero on failure, keeps the paren-safe label style and copies `dist\` to `S:\Games\Umbrella Chronicles`.
- Project root `D:\Projects\Native Ports\Umbrella-chronicles-recomp` (spaces in the path; quote everything). The repository now exists (`git`), but commits happen only when the user asks: no commit steps in this plan.
- Test command for every task: `scripts\dev.bat`. Real game: `S:\Games\Umbrella Chronicles\game\DATA\sys\main.dol`.
- Guest structure layouts (OSThread, DVDFileInfo, low-memory globals) are taken from the translated SDK code and verified in Task 1, never from memory alone.
- **Nativity rule (owner's priority: maximum nativity and performance).** Anything that behaves like emulation is allowed only as a stopgap and must be written to `docs/tech-debt.md` in the same step that adds it: a hardware-register model more complex than a plain read-back register file, a device that answers a handshake, a `Nop` that drops behaviour the game could observe, a polling/timing workaround. If a function needs a non-trivial hardware model, replace the SDK function with a native HLE at the call level instead (one line in `project/hle_map.txt`) and record the model that was avoided.

## Spec deviations and proposals (surface to the user in the hand-off)

1. **Bottom-up HLE:** the spec lists "AX, VI, DVD, OS" as HLE targets. This plan replaces only what cannot run natively (scheduler, interrupts, DVD, VI wait, privileged setup) and lets the rest of `OSInit` etc. run translated. Fewer functions to identify; more MMIO behaviour to supply.
2. **Hardware-level GX is not decided here.** M1 discards FIFO writes; the renderer plan chooses call-level or FIFO-level GX with the 145 named `gx.a` functions in hand.
3. **Audio (AX, ADX), Sofdec movies and real input are out of scope** (stubs only); each gets its own plan.
4. **Tech-debt register:** `docs/tech-debt.md` lists every emulation-like stopgap (device models, handshake answers, behaviour-dropping `Nop`s) with the native replacement planned. Created in Task 1, filled during bring-up (Task 9), summarised in Task 10.

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
docs/tech-debt.md                    emulation-like stopgaps and their native replacements
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

Names to find (from `docs/spikes/S5-identification.md`): `OSInit`, `OSReport`, `OSPanic`, `OSGetTick`, `OSJoinThread`, `DVDClose`, `WPADInit`, `WPADRead`, `KPADInit`, `KPADRead`, `KPADSetPosParam`, plus the two unattributed privileged functions `802A26F4` and `802A3EB0`. Use these commands (set `X=build\tools\xref\xref.exe --dol "S:\Games\Umbrella Chronicles\game\DATA\sys\main.dol" --cfg project\uc.cfg --names project\identified_sdk.txt`):

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
  - `namespace uc`: `void trace_dump(std::FILE*, int count = 64);`, `void trace_dump_all(std::FILE*, int count = 16);`, `void symbols_load(const std::string& path);`, `std::string symbol_name(uint32_t addr);`
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
void trace_dump(std::FILE* out, int count = 64);       // last guest functions entered by the calling thread (needs UC_TRACE)
void trace_dump_all(std::FILE* out, int count = 16);   // the same for every host thread (used by the no-progress watchdog)
void symbols_load(const std::string& path);            // "<hex> <name> ..." lines; '#' comments
std::string symbol_name(uint32_t addr);                // "name", "name+0x1C" or ""
}
```

`runtime/trace.cpp`:

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

`project/hle.cfg` is derived: add the line `project/hle.cfg` to `.gitignore`.
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

---

### Task 4: Scheduler (TDD)

**Files:**
- Create: `runtime/sched.h`, `runtime/sched.cpp`, `tests/test_sched.cpp`
- Modify: `runtime/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `uc::now_ticks`, `uc::ms_to_ticks`, `uc::ticks_to_ms` (Task 3), `uc::fatal` (Task 2), `ppc::Ctx`.
- Produces (`namespace uc`):
  - `struct ThreadExit {};` (thrown to unwind a guest thread's host stack)
  - `struct Thread { int id; uint32_t guest_addr; int priority; State state; int suspend_count; ppc::Ctx ctx; ... };` with `enum class State { Suspended, Ready, Running, Waiting, Dead }`
  - `class Sched` with: `Thread* create(uint32_t guest_addr, int priority, Body body, const ppc::Ctx* inherit = nullptr)`, `int resume(Thread*)`, `int suspend(Thread*)` (both return the previous suspend count), `void yield()`, `void block_on(uint32_t key)`, `Thread* wake_one(uint32_t key)`, `int wake_all(uint32_t key)`, `void sleep_ticks(uint64_t)`, `[[noreturn]] void exit_current(uint32_t value)`, `uint32_t join(Thread*)`, `void set_priority(Thread*, int)`, `Thread* current() const`, `Thread* find(uint32_t guest_addr) const`, `void post_at(uint64_t tick, Event)`, `void post_external(Event)`, `void safe_point()`, `bool set_interrupts(bool)` (returns the previous state), `bool in_irq() const`, `ppc::Ctx& irq_ctx()`, `void run(Thread* main_thread)`, `void shutdown()`, `set_error_handler(std::function<void(const std::string&)>)`, `set_deadlock_timeout_ms(int)`, `set_switch_hook(std::function<void(Thread*)>)`.
  - `Body = std::function<void(ppc::Ctx&)>`, `Event = std::function<void()>`.

- [ ] **Step 1: Write the failing tests**

`tests/test_sched.cpp`:
```cpp
#include "check.h"
#include "clock.h"
#include "sched.h"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
using namespace uc;

static std::vector<std::string> g_log;
static void note(const char* s) { g_log.push_back(s); }
static std::string joined() { std::string r; for (auto& s : g_log) { if (!r.empty()) r += ","; r += s; } return r; }

static void test_priorities_and_join() {
    g_log.clear(); Sched s(nullptr);
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        Thread* a = s.create(0, 10, [&](ppc::Ctx&) { note("A1"); s.yield(); note("A2"); });
        Thread* b = s.create(0, 10, [&](ppc::Ctx&) { note("B1"); note("B2"); });
        Thread* c = s.create(0, 20, [&](ppc::Ctx&) { note("C"); });
        s.resume(a); s.resume(b); s.resume(c);   // a and b are more urgent than main and run at once; c is not
        note("m1"); s.join(c); note("m2");
    });
    s.run(m);
    CHECK(joined() == "A1,A2,B1,B2,m1,C,m2");
}

static void test_round_robin() {
    g_log.clear(); Sched s(nullptr);
    Thread* m = s.create(0, 5, [&](ppc::Ctx&) {
        Thread* x1 = s.create(0, 10, [&](ppc::Ctx&) { note("x1a"); s.yield(); note("x1b"); });
        Thread* x2 = s.create(0, 10, [&](ppc::Ctx&) { note("x2a"); s.yield(); note("x2b"); });
        s.resume(x1); s.resume(x2);
        s.sleep_ticks(ms_to_ticks(20)); note("m");
    });
    s.run(m);
    CHECK(joined() == "x1a,x2a,x1b,x2b,m");
}

static void test_wake_order() {
    g_log.clear(); Sched s(nullptr);
    Thread* m = s.create(0, 1, [&](ppc::Ctx&) {
        Thread* lo = s.create(0, 20, [&](ppc::Ctx&) { s.block_on(0x1000); note("lo"); });
        Thread* hi = s.create(0, 5, [&](ppc::Ctx&) { s.block_on(0x1000); note("hi"); });
        Thread* mid = s.create(0, 10, [&](ppc::Ctx&) { s.block_on(0x1000); note("mid"); });
        s.resume(lo); s.resume(hi); s.resume(mid);
        s.sleep_ticks(ms_to_ticks(10));                 // let all three block
        CHECK(s.wake_one(0x1000) == hi);
        CHECK(s.wake_one(0x1000) == mid);
        CHECK(s.wake_one(0x1000) == lo);
        CHECK(s.wake_one(0x1000) == nullptr);
        note("m");
        s.sleep_ticks(ms_to_ticks(10));
    });
    s.run(m);
    CHECK(joined() == "m,hi,mid,lo");
}

static void test_wake_preempts() {
    g_log.clear(); Sched s(nullptr);
    Thread* m = s.create(0, 20, [&](ppc::Ctx&) {
        Thread* h = s.create(0, 5, [&](ppc::Ctx&) { s.block_on(0x2000); note("H"); });
        s.resume(h);                       // H preempts, blocks, main continues
        note("m1"); s.wake_all(0x2000);    // H is more urgent: it runs before m2
        note("m2");
    });
    s.run(m);
    CHECK(joined() == "m1,H,m2");
}

static void test_busy_wait_sees_events() {
    Sched s(nullptr); std::atomic<bool> flag{false}; int spins = 0;
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        s.post_at(now_ticks() + ms_to_ticks(10), [&] { flag = true; });
        while (!flag) { ++spins; s.safe_point(); }     // a guest spin loop that polls the clock or a flag
    });
    s.run(m);
    CHECK(flag && spins > 0);
}

static void test_events_wait_for_interrupts() {
    Sched s(nullptr); bool fired = false; bool seen_while_disabled = true;
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        CHECK(s.set_interrupts(false) == true);        // returns the previous state
        s.post_at(now_ticks(), [&] { fired = true; });
        s.safe_point();
        seen_while_disabled = fired;                   // nothing may be delivered while interrupts are off
        CHECK(s.set_interrupts(true) == false);        // enabling is a safe point: the event runs now
    });
    s.run(m);
    CHECK(!seen_while_disabled && fired);
}

static void test_external_wakes_idle() {
    g_log.clear(); Sched s(nullptr); std::thread t;
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        t = std::thread([&] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); s.post_external([&] { s.wake_all(0x3000); }); });
        s.block_on(0x3000); note("woken");           // every guest thread is blocked: the idle driver must pick up the post
    });
    s.run(m); t.join();
    CHECK(joined() == "woken");
}

static void test_deadlock_report() {
    g_log.clear(); Sched s(nullptr); std::string msg;
    s.set_deadlock_timeout_ms(100);
    s.set_error_handler([&](const std::string& m) { msg = m; s.shutdown(); });
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) { s.block_on(0xABCD); note("never"); });
    s.run(m);
    CHECK(msg.find("deadlock") != std::string::npos && msg.find("0000ABCD") != std::string::npos);
    CHECK(g_log.empty());
}

static void test_irq_block_is_fatal() {
    Sched s(nullptr); std::string msg;
    s.set_error_handler([&](const std::string& m) { msg = m; s.shutdown(); });
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        s.post_at(now_ticks(), [&] { s.block_on(0x4000); });
        s.sleep_ticks(ms_to_ticks(50));
    });
    s.run(m);
    CHECK(msg.find("interrupt context") != std::string::npos);
}

static void test_exit_and_join_values() {
    g_log.clear(); Sched s(nullptr);
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        Thread* t = s.create(0, 10, [&](ppc::Ctx&) { note("t"); s.exit_current(7); note("unreachable"); });
        Thread* u = s.create(0, 10, [&](ppc::Ctx& c) { c.r[3] = 9; });   // returning from the entry function exits with r3
        s.resume(t); s.resume(u);
        CHECK(s.join(t) == 7); CHECK(s.join(u) == 9); note("m");
    });
    s.run(m);
    CHECK(joined() == "t,m");
}

static void test_suspend_resume() {
    g_log.clear(); Sched s(nullptr);
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        Thread* t = s.create(0, 10, [&](ppc::Ctx&) { note("t"); });
        CHECK(s.resume(t) == 1);   // created suspended; resume returns the previous count
    });
    s.run(m);
    CHECK(joined() == "t");
    g_log.clear(); Sched s2(nullptr);
    Thread* m2 = s2.create(0, 5, [&](ppc::Ctx&) {
        Thread* t = s2.create(0, 10, [&](ppc::Ctx&) { note("t"); });
        s2.resume(t);                    // less urgent than m2: ready but not running
        CHECK(s2.suspend(t) == 0);
        s2.sleep_ticks(ms_to_ticks(10)); note("m");
        CHECK(s2.resume(t) == 1);
        s2.sleep_ticks(ms_to_ticks(10)); note("m2");
    });
    s2.run(m2);
    CHECK(joined() == "m,t,m2");
}

static void test_sleep_is_at_least_requested() {
    Sched s(nullptr); uint64_t waited = 0;
    Thread* m = s.create(0, 16, [&](ppc::Ctx&) {
        uint64_t a = now_ticks(); s.sleep_ticks(ms_to_ticks(25)); waited = ticks_to_ms(now_ticks() - a);
    });
    s.run(m);
    CHECK(waited >= 24 && waited < 400);
}

int main() {
    test_priorities_and_join(); test_round_robin(); test_wake_order(); test_wake_preempts();
    test_busy_wait_sees_events(); test_events_wait_for_interrupts(); test_external_wakes_idle();
    test_deadlock_report(); test_irq_block_is_fatal(); test_exit_and_join_values();
    test_suspend_resume(); test_sleep_is_at_least_requested();
    return 0;
}
```
Add `test_sched` to the `test_support` loop (the one with `test_ppc_*`). Run: `scripts\dev.bat` — Expected: FAIL (`sched.h` not found).

- [ ] **Step 2: Implement `runtime/sched.h`**

```cpp
#pragma once
#include "ppc_rt.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace uc {

struct ThreadExit {};   // thrown to unwind a guest thread's host stack; carries no guest-visible meaning

struct Thread {
    enum class State { Suspended, Ready, Running, Waiting, Dead };
    int id = 0;
    uint32_t guest_addr = 0;   // OSThread* in guest memory (0 for host-made threads)
    int priority = 16;         // 0 is the most urgent, 31 the least
    State state = State::Suspended;
    int suspend_count = 1;     // OSCreateThread leaves a thread suspended
    uint32_t wait_key = 0;
    uint32_t exit_value = 0;
    int64_t seq = 0;           // FIFO order inside one priority
    bool started = false;
    ppc::Ctx ctx{};
    std::function<void(ppc::Ctx&)> body;
    std::condition_variable cv;
};

class Sched {
public:
    using Body = std::function<void(ppc::Ctx&)>;
    using Event = std::function<void()>;

    explicit Sched(uint8_t* guest_mem, size_t host_stack = 8u << 20);
    ~Sched();
    Sched(const Sched&) = delete;
    Sched& operator=(const Sched&) = delete;

    Thread* create(uint32_t guest_addr, int priority, Body body, const ppc::Ctx* inherit = nullptr);
    int resume(Thread* t);
    int suspend(Thread* t);
    void yield();
    void block_on(uint32_t key);
    Thread* wake_one(uint32_t key);
    int wake_all(uint32_t key);
    void sleep_ticks(uint64_t ticks);
    [[noreturn]] void exit_current(uint32_t value);
    uint32_t join(Thread* t);
    void set_priority(Thread* t, int priority);
    Thread* current() const { return running_; }
    Thread* find(uint32_t guest_addr) const;

    // Events run on the thread that holds the baton, only while interrupts are enabled (the "interrupt context").
    void post_at(uint64_t tick, Event fn);       // baton holder only
    void post_external(Event fn);                // any host thread
    void safe_point();                           // deliver due events, then let a more urgent ready thread run
    bool set_interrupts(bool enabled);           // returns the previous state; enabling is a safe point
    bool interrupts_enabled() const { return ee_; }
    bool in_irq() const { return in_irq_; }
    ppc::Ctx& irq_ctx() { return irq_ctx_; }

    void run(Thread* main_thread);               // returns when the main thread has exited or shutdown() was called
    void shutdown();
    void set_error_handler(std::function<void(const std::string&)> h) { err_ = std::move(h); }
    void set_deadlock_timeout_ms(int ms) { deadlock_ms_ = ms; }
    void set_switch_hook(std::function<void(Thread*)> h) { switch_hook_ = std::move(h); }

    void thread_main(Thread* t);                 // host thread entry (internal)

private:
    static constexpr uint32_t join_key(const Thread* t) { return 0xF0000000u + uint32_t(t->id); }
    static constexpr uint32_t sleep_key(const Thread* t) { return 0xF8000000u + uint32_t(t->id); }
    void make_ready(Thread* t, bool front = false);
    Thread* pick_ready();
    void wake(Thread* t);
    void preempt_check();
    void reschedule(Thread* self);
    void handoff(Thread* self, Thread* next);
    void finish(Thread* self, uint32_t value);
    void start_host(Thread* t);
    void deliver_events();
    void idle_wait();
    [[noreturn]] void fail(const std::string& msg);
    std::string dump_threads() const;

    uint8_t* mem_;
    size_t host_stack_;
    std::vector<std::unique_ptr<Thread>> threads_;
    std::map<uint32_t, std::vector<Thread*>> waitq_;
    std::multimap<uint64_t, Event> timed_;
    std::vector<Event> ext_q_;
    std::mutex baton_m_, ext_m_, done_m_;
    std::condition_variable ext_cv_, done_cv_;
    std::vector<void*> handles_;
    Thread* running_ = nullptr;
    Thread* main_ = nullptr;
    ppc::Ctx irq_ctx_{};
    std::atomic<bool> shutdown_{false}, main_done_{false};
    bool ee_ = true, in_irq_ = false;
    int next_id_ = 0, deadlock_ms_ = 5000;
    int64_t front_seq_ = 0, back_seq_ = 0;
    std::function<void(const std::string&)> err_;
    std::function<void(Thread*)> switch_hook_;
};

}  // namespace uc
```

- [ ] **Step 3: Implement `runtime/sched.cpp`**

```cpp
#include "sched.h"
#include "clock.h"
#include "fatal.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace uc {

namespace {
struct Tramp { Sched* s; Thread* t; };
DWORD WINAPI tramp(LPVOID p) {
    std::unique_ptr<Tramp> tp(static_cast<Tramp*>(p));
    tp->s->thread_main(tp->t);
    return 0;
}
}  // namespace

Sched::Sched(uint8_t* mem, size_t host_stack) : mem_(mem), host_stack_(host_stack) { irq_ctx_.mem = mem; }

Sched::~Sched() {
    shutdown();
    for (void* h : handles_) { WaitForSingleObject(static_cast<HANDLE>(h), 2000); CloseHandle(static_cast<HANDLE>(h)); }
}

Thread* Sched::create(uint32_t guest_addr, int priority, Body body, const ppc::Ctx* inherit) {
    auto t = std::make_unique<Thread>();
    t->id = ++next_id_;
    t->guest_addr = guest_addr;
    t->priority = priority;
    t->body = std::move(body);
    if (inherit) t->ctx = *inherit;
    t->ctx.mem = mem_;
    threads_.push_back(std::move(t));
    return threads_.back().get();
}

Thread* Sched::find(uint32_t guest_addr) const {   // newest first, finished threads included: joining a finished thread returns its value
    for (auto it = threads_.rbegin(); it != threads_.rend(); ++it) if ((*it)->guest_addr == guest_addr) return it->get();
    return nullptr;
}

void Sched::make_ready(Thread* t, bool front) {
    t->state = Thread::State::Ready;
    t->seq = front ? --front_seq_ : ++back_seq_;
}

Thread* Sched::pick_ready() {
    Thread* best = nullptr;
    for (const auto& up : threads_) {
        Thread* t = up.get();
        if (t->state != Thread::State::Ready) continue;
        if (!best || t->priority < best->priority || (t->priority == best->priority && t->seq < best->seq)) best = t;
    }
    return best;
}

void Sched::wake(Thread* t) {
    t->wait_key = 0;
    if (t->suspend_count > 0) t->state = Thread::State::Suspended; else make_ready(t);
}

void Sched::preempt_check() {
    if (in_irq_ || !running_ || running_->state != Thread::State::Running) return;
    Thread* best = pick_ready();
    if (best && best->priority < running_->priority) {
        Thread* self = running_;
        make_ready(self, true);   // a preempted thread keeps its place at the front of its priority
        reschedule(self);
    }
}

int Sched::resume(Thread* t) {
    int prev = t->suspend_count;
    if (t->suspend_count > 0) --t->suspend_count;
    if (t->suspend_count == 0 && t->state == Thread::State::Suspended) { make_ready(t); preempt_check(); }
    return prev;
}

int Sched::suspend(Thread* t) {
    int prev = t->suspend_count++;
    if (t->state == Thread::State::Ready) t->state = Thread::State::Suspended;
    else if (t == running_ && !in_irq_) { t->state = Thread::State::Suspended; reschedule(t); }
    return prev;   // a waiting thread stays waiting and becomes suspended when it is woken
}

void Sched::yield() {
    if (in_irq_) return;
    Thread* self = running_;
    make_ready(self);
    reschedule(self);
}

void Sched::block_on(uint32_t key) {
    if (in_irq_) fail("blocking call from interrupt context (wait key " + [&] { char b[16]; std::snprintf(b, sizeof b, "%08X", key); return std::string(b); }() + ")");
    Thread* self = running_;
    self->state = Thread::State::Waiting;
    self->wait_key = key;
    waitq_[key].push_back(self);
    reschedule(self);
}

Thread* Sched::wake_one(uint32_t key) {
    auto it = waitq_.find(key);
    if (it == waitq_.end() || it->second.empty()) return nullptr;
    auto& q = it->second;
    size_t best = 0;
    for (size_t i = 1; i < q.size(); ++i) if (q[i]->priority < q[best]->priority) best = i;
    Thread* t = q[best];
    q.erase(q.begin() + long(best));
    if (q.empty()) waitq_.erase(it);
    wake(t);
    preempt_check();
    return t;
}

int Sched::wake_all(uint32_t key) {
    auto it = waitq_.find(key);
    if (it == waitq_.end()) return 0;
    std::vector<Thread*> q = std::move(it->second);
    waitq_.erase(it);
    for (Thread* t : q) wake(t);
    preempt_check();
    return int(q.size());
}

void Sched::sleep_ticks(uint64_t ticks) {
    uint32_t key = sleep_key(running_);
    post_at(now_ticks() + ticks, [this, key] { wake_all(key); });
    block_on(key);
}

void Sched::set_priority(Thread* t, int priority) {
    t->priority = priority;
    preempt_check();
}

uint32_t Sched::join(Thread* t) {
    while (t->state != Thread::State::Dead) block_on(join_key(t));
    return t->exit_value;
}

void Sched::finish(Thread* self, uint32_t value) {
    self->exit_value = value;
    self->state = Thread::State::Dead;
    if (self == main_) { { std::lock_guard<std::mutex> lk(done_m_); main_done_ = true; } done_cv_.notify_all(); }
    wake_all(join_key(self));
    reschedule(self);
}

void Sched::exit_current(uint32_t value) {
    finish(running_, value);
    throw ThreadExit{};
}

void Sched::post_at(uint64_t tick, Event fn) { timed_.emplace(tick, std::move(fn)); }

void Sched::post_external(Event fn) {
    { std::lock_guard<std::mutex> lk(ext_m_); ext_q_.push_back(std::move(fn)); }
    ext_cv_.notify_all();
}

void Sched::deliver_events() {
    if (in_irq_ || !ee_) return;
    std::vector<Event> ext;
    { std::lock_guard<std::mutex> lk(ext_m_); ext.swap(ext_q_); }
    for (auto& e : ext) timed_.emplace(0, std::move(e));
    while (!timed_.empty() && timed_.begin()->first <= now_ticks()) {
        Event e = std::move(timed_.begin()->second);
        timed_.erase(timed_.begin());
        in_irq_ = true;
        e();
        in_irq_ = false;
    }
}

void Sched::safe_point() {
    deliver_events();
    preempt_check();
}

bool Sched::set_interrupts(bool enabled) {
    bool prev = ee_;
    ee_ = enabled;
    if (enabled && !prev) safe_point();
    return prev;
}

std::string Sched::dump_threads() const {
    std::string s;
    static const char* names[] = {"suspended", "ready", "running", "waiting", "dead"};
    for (const auto& t : threads_) {
        if (t->state == Thread::State::Dead) continue;
        char b[160];
        std::snprintf(b, sizeof b, "  thread %d (OSThread %08X) priority %d %s wait key %08X\n", t->id, t->guest_addr, t->priority, names[int(t->state)], t->wait_key);
        s += b;
    }
    return s;
}

void Sched::fail(const std::string& msg) {
    if (err_) { err_(msg); throw ThreadExit{}; }
    fatal("%s", msg.c_str());
}

void Sched::idle_wait() {
    std::unique_lock<std::mutex> lk(ext_m_);
    if (!ext_q_.empty() || shutdown_) return;
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlock_ms_);
    bool timed = false;
    if (ee_ && !timed_.empty()) {   // with interrupts off nothing can ever be delivered: that is a deadlock, not a wait
        uint64_t t = timed_.begin()->first, now = now_ticks();
        until = std::chrono::steady_clock::now() + std::chrono::milliseconds(t > now ? ticks_to_ms(t - now) + 1 : 0);
        timed = true;
    }
    bool woke = ext_cv_.wait_until(lk, until, [&] { return !ext_q_.empty() || shutdown_; });
    if (!woke && !timed) {
        lk.unlock();
        fail("deadlock: all guest threads are blocked and no event is pending\n" + dump_threads());
    }
}

void Sched::reschedule(Thread* self) {
    for (;;) {
        if (shutdown_) {
            if (self->state == Thread::State::Dead) return;
            throw ThreadExit{};
        }
        deliver_events();
        if (Thread* n = pick_ready()) { handoff(self, n); return; }
        if (main_done_) return;
        idle_wait();
    }
}

void Sched::handoff(Thread* self, Thread* next) {
    if (next == self) { self->state = Thread::State::Running; return; }
    next->state = Thread::State::Running;
    if (switch_hook_) switch_hook_(next);
    {
        std::lock_guard<std::mutex> lk(baton_m_);
        running_ = next;
        if (!next->started) start_host(next);
    }
    next->cv.notify_one();
    if (self->state == Thread::State::Dead) return;
    std::unique_lock<std::mutex> lk(baton_m_);
    self->cv.wait(lk, [&] { return running_ == self || shutdown_; });
    if (running_ != self) throw ThreadExit{};
}

void Sched::start_host(Thread* t) {
    t->started = true;
    HANDLE h = CreateThread(nullptr, host_stack_, tramp, new Tramp{this, t}, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) fatal("cannot create a host thread for guest thread %d", t->id);
    handles_.push_back(h);
}

void Sched::thread_main(Thread* t) {
    {
        std::unique_lock<std::mutex> lk(baton_m_);
        t->cv.wait(lk, [&] { return running_ == t || shutdown_; });
        if (running_ != t) return;
    }
    try {
        t->body(t->ctx);
        finish(t, t->ctx.r[3]);   // returning from the entry function is OSExitThread(r3)
    } catch (const ThreadExit&) {
    }
}

void Sched::run(Thread* m) {
    main_ = m;
    m->suspend_count = 0;
    if (switch_hook_) switch_hook_(m);
    {
        std::lock_guard<std::mutex> lk(baton_m_);
        m->state = Thread::State::Running;
        running_ = m;
        start_host(m);
    }
    m->cv.notify_one();
    std::unique_lock<std::mutex> lk(done_m_);
    done_cv_.wait(lk, [&] { return main_done_ || shutdown_; });
}

void Sched::shutdown() {
    { std::lock_guard<std::mutex> lk(baton_m_); shutdown_ = true; }
    for (auto& t : threads_) t->cv.notify_all();
    { std::lock_guard<std::mutex> lk(ext_m_); }
    ext_cv_.notify_all();
    { std::lock_guard<std::mutex> lk(done_m_); }
    done_cv_.notify_all();
}

}  // namespace uc
```
Add `sched.cpp` to `uc_runtime`.

- [ ] **Step 4: Run to verify**

Run: `scripts\dev.bat` — Expected: `test_sched` PASSED within a few seconds, everything else green. A failing check names the scenario; fix `sched.cpp` (the expected logs follow OS scheduling rules: most urgent first, FIFO inside a priority, a woken more-urgent thread preempts the waker). If a test hangs, run it alone with `ctest -R test_sched --timeout 20` and read which scenario's thread is blocked (`dump_threads`).

---

### Task 5: OS HLE — threads, sync, interrupts, time, alarms, report

**Files:**

- Create: `runtime/hle/os_layout.h`, `runtime/hle/os.h`, `runtime/hle/os_thread.cpp`, `runtime/hle/os_sync.cpp`, `runtime/hle/os_misc.cpp`, `runtime/hle/format_guest.cpp`, `tests/test_os_hle.cpp`
- Modify: `runtime/CMakeLists.txt`, `tests/CMakeLists.txt`, `project/hle_map.txt`

**Interfaces:**

- Consumes: `uc::Sched`, `uc::call_guest`, `uc::call_indirect`, `uc::now_ticks`, `ppc::rd32/wr32/wr16`, offsets from `docs/spikes/R0-layouts.md` (Task 1).
- Produces:
  - `void uc::os_hle_init(Sched& s, uint8_t* guest_mem, uint32_t dummy_context_addr);` `Sched& uc::os_sched();` `uint32_t uc::os_dummy_context();`
  - HLE functions (all `void(ppc::Ctx&)`): `hle_OSCreateThread hle_OSResumeThread hle_OSSuspendThread hle_OSSleepThread hle_OSWakeupThread hle_OSYieldThread hle_OSExitThread hle_OSJoinThread hle_OSSetThreadPriority hle_OSInitMutex hle_OSLockMutex hle_OSUnlockMutex hle_OSInitMessageQueue hle_OSSendMessage hle_OSReceiveMessage hle_OSJamMessage hle_OSDisableInterrupts hle_OSEnableInterrupts hle_OSRestoreInterrupts hle_OSGetTime hle_OSGetTick hle_OSSetAlarm hle_OSSetPeriodicAlarm hle_OSCancelAlarm hle_OSReport hle_OSPanic`.
  - `std::string uc::format_guest(ppc::Ctx& c, uint32_t fmt_addr, int first_gpr);` (printf over guest varargs: integers from `r<first_gpr>…r10`, then the caller's stack at `r1+8`; doubles from `f1…f8`).

- [ ] **Step 1: Fix the layout constants from Task 1**

`runtime/hle/os_layout.h` is the only file that holds guest structure offsets. Fill it from `docs/spikes/R0-layouts.md` (the defaults below are the values to confirm or replace):

```cpp
#pragma once
#include <cstdint>
namespace uc::layout {
// OSThread (RVL SDK). Confirm every value against the disassembly recorded in R0-layouts.md.
constexpr uint32_t kThreadState = 0x2C8;      // u16: 1 ready, 2 running, 4 waiting, 8 moribund
constexpr uint32_t kThreadAttr = 0x2CA;       // u16
constexpr uint32_t kThreadSuspend = 0x2CC;    // s32
constexpr uint32_t kThreadPriority = 0x2D0;   // s32
constexpr uint32_t kThreadBasePriority = 0x2D4;
constexpr uint32_t kThreadStackBase = 0x304;  // u32 top of stack
constexpr uint32_t kThreadStackEnd = 0x308;   // u32 lowest address (holds the 0xDEADBABE canary)
constexpr uint32_t kCurrentThreadPtr = 0x800000E4;   // low memory: OSGetCurrentThread() reads this
}
```

Run `scripts\dev.bat` after editing — Expected: still green.

- [ ] **Step 2: Write the failing tests**

`tests/test_os_hle.cpp`:

```cpp
#include "check.h"
#include "clock.h"
#include "dispatch.h"
#include "guest_call.h"
#include "hle/os.h"
#include "ppc_fixture.h"
#include "sched.h"
#include <cstring>
#include <string>
#include <vector>
using namespace ppc;

// hle entry points (declared by the generated decl.h in the real build)
void hle_OSCreateThread(Ctx&); void hle_OSResumeThread(Ctx&); void hle_OSExitThread(Ctx&); void hle_OSJoinThread(Ctx&);
void hle_OSSleepThread(Ctx&); void hle_OSWakeupThread(Ctx&); void hle_OSYieldThread(Ctx&);
void hle_OSInitMutex(Ctx&); void hle_OSLockMutex(Ctx&); void hle_OSUnlockMutex(Ctx&);
void hle_OSInitMessageQueue(Ctx&); void hle_OSSendMessage(Ctx&); void hle_OSReceiveMessage(Ctx&);
void hle_OSDisableInterrupts(Ctx&); void hle_OSRestoreInterrupts(Ctx&); void hle_OSEnableInterrupts(Ctx&);
void hle_OSGetTime(Ctx&); void hle_OSGetTick(Ctx&); void hle_OSSetAlarm(Ctx&); void hle_OSSetPeriodicAlarm(Ctx&); void hle_OSCancelAlarm(Ctx&);

static std::vector<std::string> g_log;
static void note(const std::string& s) { g_log.push_back(s); }
static std::string joined() { std::string r; for (auto& s : g_log) { if (!r.empty()) r += ","; r += s; } return r; }

// "Guest" functions: plain C++ functions registered in the dispatch table at fake guest addresses.
static void guest_worker(Ctx& c) { note("worker(" + std::to_string(c.r[3]) + ")"); c.r[3] = c.r[3] * 2; }          // returns param*2
static void guest_alarm(Ctx& c) { note("alarm(" + std::to_string(c.r[3]) + ")"); }
static void guest_cb(Ctx& c) { note("cb"); }

static void call(Ctx& c, void (*fn)(Ctx&), std::initializer_list<uint32_t> args) { int i = 3; for (uint32_t a : args) c.r[i++] = a; fn(c); }

int main() {
    Fix f; uc::Sched s(f.c.mem);
    static const uc::FnEntry table[] = {{0x80100000, guest_worker}, {0x80100004, guest_alarm}, {0x80100008, guest_cb}};
    uc::dispatch_init(table, 3, 0x80100000, 0x8010000C);
    uc::os_hle_init(s, f.c.mem, 0x817F0000);
    s.irq_ctx().r[1] = 0x817EFF00;

    uc::Thread* m = s.create(0, 16, [&](Ctx& c) {
        // thread create/resume/join: entry gets r3 = param; join returns the entry's r3
        call(c, hle_OSCreateThread, {0x80300000, 0x80100000, 21, 0x80420000, 0x4000, 10, 0});
        CHECK(c.r[3] == 1);
        CHECK(rd32(c, 0x80420000 - 0x4000) == 0xDEADBABE);           // stack canary at the lowest address
        call(c, hle_OSResumeThread, {0x80300000});
        CHECK(c.r[3] == 1);                                          // previous suspend count; the new thread was more urgent and ran already
        call(c, hle_OSJoinThread, {0x80300000, 0x80310000});
        CHECK(c.r[3] == 1 && rd32(c, 0x80310000) == 42);

        // mutex: recursive for the owner; a contender waits for the unlock
        call(c, hle_OSInitMutex, {0x80320000});
        call(c, hle_OSLockMutex, {0x80320000}); call(c, hle_OSLockMutex, {0x80320000});
        call(c, hle_OSCreateThread, {0x80301000, 0x80100000, 1, 0x80440000, 0x4000, 5, 0});
        call(c, hle_OSUnlockMutex, {0x80320000});
        call(c, hle_OSUnlockMutex, {0x80320000});

        // message queue: blocking receive is woken by a send; non-blocking receive on empty fails
        call(c, hle_OSInitMessageQueue, {0x80330000, 0x80331000, 2});
        call(c, hle_OSReceiveMessage, {0x80330000, 0x80332000, 0});      // OS_MESSAGE_NOBLOCK
        CHECK(c.r[3] == 0);
        call(c, hle_OSSendMessage, {0x80330000, 77, 0});  CHECK(c.r[3] == 1);
        call(c, hle_OSSendMessage, {0x80330000, 78, 0});  CHECK(c.r[3] == 1);
        call(c, hle_OSSendMessage, {0x80330000, 79, 0});  CHECK(c.r[3] == 0);   // full, non-blocking
        call(c, hle_OSReceiveMessage, {0x80330000, 0x80332000, 1});
        CHECK(c.r[3] == 1 && rd32(c, 0x80332000) == 77);

        // interrupts: Disable returns the previous state, Restore returns the state it replaced
        call(c, hle_OSDisableInterrupts, {});         CHECK(c.r[3] == 1);
        call(c, hle_OSDisableInterrupts, {});         CHECK(c.r[3] == 0);
        call(c, hle_OSRestoreInterrupts, {1});        CHECK(c.r[3] == 0);
        call(c, hle_OSEnableInterrupts, {});          CHECK(c.r[3] == 1);

        // time: 64-bit OSGetTime in r3:r4 never goes backwards, OSGetTick is its low word
        call(c, hle_OSGetTime, {});
        uint64_t t1 = uint64_t(c.r[3]) << 32 | c.r[4];
        call(c, hle_OSGetTick, {});
        uint64_t t2 = uint64_t(c.r[3]);
        CHECK(t1 != 0 && (t2 >= (t1 & 0xFFFFFFFF) || t2 < 0x1000));

        // alarm: handler runs in interrupt context with r3 = the alarm; cancel stops a pending one
        g_log.clear();
        uint64_t rel = uc::ms_to_ticks(10);
        call(c, hle_OSSetAlarm, {0x80340000, uint32_t(rel >> 32), uint32_t(rel), 0x80100004});
        call(c, hle_OSSetAlarm, {0x80341000, uint32_t(rel >> 32), uint32_t(rel), 0x80100004});
        call(c, hle_OSCancelAlarm, {0x80341000});
        s.sleep_ticks(uc::ms_to_ticks(30));
        CHECK(joined() == "alarm(" + std::to_string(0x80340000) + ")");
        // periodic alarm fires repeatedly until cancelled
        g_log.clear();
        uint64_t start = uc::now_ticks() + uc::ms_to_ticks(5), period = uc::ms_to_ticks(10);
        call(c, hle_OSSetPeriodicAlarm, {0x80342000, uint32_t(start >> 32), uint32_t(start), uint32_t(period >> 32), uint32_t(period), 0x80100004});
        s.sleep_ticks(uc::ms_to_ticks(45));
        call(c, hle_OSCancelAlarm, {0x80342000});
        size_t n = g_log.size();
        CHECK(n >= 3 && n <= 6);
        s.sleep_ticks(uc::ms_to_ticks(30));
        CHECK(g_log.size() == n);
    });
    g_log.clear();
    s.run(m);
    return 0;
}
```

For the mutex scenario the log shows `worker(1)` ran only after the final unlock because the contender (prio 5) is more urgent than main (16): it blocks on the held mutex only if the worker function locks it; the guest worker above does not lock, so this part only verifies that create/resume with a held mutex does not misbehave. A dedicated contention check lives in `test_sched`; keep this test as an API-shape test.
Also add a `format_guest` test in the same file before `s.run`:

```cpp
    {   // printf over guest varargs
        Ctx& c = f.c;
        const char* fmt = "%s=%d (%x) %c %5.2f %%";
        for (size_t i = 0; i <= strlen(fmt); ++i) wr8(c, 0x80350000 + uint32_t(i), uint32_t(fmt[i]));
        const char* name = "val";
        for (size_t i = 0; i <= strlen(name); ++i) wr8(c, 0x80350100 + uint32_t(i), uint32_t(name[i]));
        c.r[4] = 0x80350100; c.r[5] = uint32_t(-5); c.r[6] = 0xBEEF; c.r[7] = 'Z'; c.f[1] = 3.14159;
        CHECK(uc::format_guest(c, 0x80350000, 4) == "val=-5 (beef) Z  3.14 %");
    }
```

Add `test_os_hle` to the `test_support` loop. Run `scripts\dev.bat` — Expected: FAIL (`hle/os.h` not found).

- [ ] **Step 3: Implement**

`runtime/hle/os.h`:

```cpp
#pragma once
#include "ppc_rt.h"
#include "sched.h"
#include <string>
namespace uc {
void os_hle_init(Sched& s, uint8_t* guest_mem, uint32_t dummy_context_addr);   // dummy_context_addr: guest memory handed to alarm handlers as their OSContext*
uint32_t os_dummy_context();
Sched& os_sched();
std::string format_guest(ppc::Ctx& c, uint32_t fmt_addr, int first_gpr);
}
```

`runtime/hle/os_thread.cpp`:

```cpp
#include "hle/os.h"
#include "dispatch.h"
#include "hle/os_layout.h"

namespace uc {
static Sched* g_sched;
static uint32_t g_dummy_ctx;
static ppc::Ctx g_memctx;   // only used to write guest memory from the scheduler hook
void os_hle_init(Sched& s, uint8_t* guest_mem, uint32_t dummy_context_addr) {
    g_sched = &s;
    g_dummy_ctx = dummy_context_addr;
    g_memctx.mem = guest_mem;
    s.set_switch_hook([](Thread* t) { ppc::wr32(g_memctx, layout::kCurrentThreadPtr, t->guest_addr); });   // OSGetCurrentThread() reads this word
}
Sched& os_sched() { return *g_sched; }
uint32_t os_dummy_context() { return g_dummy_ctx; }
}  // namespace uc

using namespace uc;

// OSCreateThread(thread, entry, param, stack_top, stack_size, priority, attr) -> 1
void hle_OSCreateThread(ppc::Ctx& c) {
    uint32_t th = c.r[3], entry = c.r[4], param = c.r[5], top = c.r[6], size = c.r[7];
    int prio = int(c.r[8]);
    ppc::wr16(c, th + layout::kThreadAttr, c.r[9]);
    ppc::wr32(c, th + layout::kThreadPriority, uint32_t(prio));
    ppc::wr32(c, th + layout::kThreadBasePriority, uint32_t(prio));
    ppc::wr32(c, th + layout::kThreadStackBase, top);
    ppc::wr32(c, th + layout::kThreadStackEnd, top - size);
    ppc::wr32(c, top - size, 0xDEADBABE);   // stack-overflow canary the SDK checks
    // The new thread inherits r2/r13 (small-data bases), GQRs and FPSCR from its creator, as OSContextInit does.
    os_sched().create(th, prio, [=](ppc::Ctx& tc) {
        tc.r[1] = top - 8;
        ppc::wr32(tc, tc.r[1], 0);
        tc.r[3] = param;
        tc.lr = 0;
        call_indirect(tc, entry);   // returns with the entry function's r3 as the thread's exit value
    }, &c);
    c.r[3] = 1;
}

void hle_OSResumeThread(ppc::Ctx& c) {
    Thread* t = os_sched().find(c.r[3]);
    c.r[3] = t ? uint32_t(os_sched().resume(t)) : uint32_t(-1);
}
void hle_OSSuspendThread(ppc::Ctx& c) {
    Thread* t = os_sched().find(c.r[3]);
    c.r[3] = t ? uint32_t(os_sched().suspend(t)) : uint32_t(-1);
}
void hle_OSSleepThread(ppc::Ctx& c) { os_sched().block_on(c.r[3]); }          // r3 = OSThreadQueue*
void hle_OSWakeupThread(ppc::Ctx& c) { os_sched().wake_all(c.r[3]); }
void hle_OSYieldThread(ppc::Ctx&) { os_sched().yield(); }
void hle_OSExitThread(ppc::Ctx& c) { os_sched().exit_current(c.r[3]); }
void hle_OSJoinThread(ppc::Ctx& c) {                                          // (thread, void** value) -> 1
    Thread* t = os_sched().find(c.r[3]);
    if (!t) {   // already finished and forgotten: nothing to wait for
        c.r[3] = 0;
        return;
    }
    uint32_t v = os_sched().join(t);
    if (c.r[4]) ppc::wr32(c, c.r[4], v);
    c.r[3] = 1;
}
void hle_OSSetThreadPriority(ppc::Ctx& c) {
    Thread* t = os_sched().find(c.r[3]);
    if (!t) { c.r[3] = 0; return; }
    ppc::wr32(c, c.r[3] + layout::kThreadPriority, c.r[4]);
    os_sched().set_priority(t, int(c.r[4]));
    c.r[3] = 1;
}
```

`Thread::guest_addr` of the host-made main thread is 0 in unit tests; the boot driver (Task 8) creates the main guest thread with the address of a real `OSThread` slot so `OSGetCurrentThread` returns a valid pointer.

`runtime/hle/os_sync.cpp`:

```cpp
#include "hle/os.h"
#include <deque>
#include <map>

using namespace uc;

namespace {
struct MutexState { Thread* owner = nullptr; int count = 0; };
struct QueueState { std::deque<uint32_t> msgs; uint32_t cap = 0; };
std::map<uint32_t, MutexState> g_mutex;
std::map<uint32_t, QueueState> g_queue;
constexpr uint32_t recv_key(uint32_t q) { return q; }
constexpr uint32_t send_key(uint32_t q) { return q + 1; }   // unaligned, so it never equals an object address
}

// shortcut: no priority inheritance; add it when a bring-up trace shows a priority inversion
void hle_OSInitMutex(ppc::Ctx& c) { g_mutex[c.r[3]] = {}; }
void hle_OSLockMutex(ppc::Ctx& c) {
    uint32_t key = c.r[3];
    Thread* me = os_sched().current();
    for (;;) {
        MutexState& m = g_mutex[key];
        if (!m.owner || m.owner == me) { m.owner = me; ++m.count; return; }
        os_sched().block_on(key);
    }
}
void hle_OSUnlockMutex(ppc::Ctx& c) {
    uint32_t key = c.r[3];
    MutexState& m = g_mutex[key];
    if (m.owner != os_sched().current()) return;
    if (--m.count == 0) { m.owner = nullptr; os_sched().wake_one(key); }
}

void hle_OSInitMessageQueue(ppc::Ctx& c) { g_queue[c.r[3]] = {{}, c.r[5]}; }   // (queue, array, count)
static void send(ppc::Ctx& c, bool front) {                                     // (queue, msg, flags); OS_MESSAGE_BLOCK = 1
    uint32_t q = c.r[3], msg = c.r[4], flags = c.r[5];
    for (;;) {
        QueueState& s = g_queue[q];
        if (s.msgs.size() < s.cap) {
            if (front) s.msgs.push_front(msg); else s.msgs.push_back(msg);
            os_sched().wake_one(recv_key(q));
            c.r[3] = 1;
            return;
        }
        if (!(flags & 1)) { c.r[3] = 0; return; }
        os_sched().block_on(send_key(q));
    }
}
void hle_OSSendMessage(ppc::Ctx& c) { send(c, false); }
void hle_OSJamMessage(ppc::Ctx& c) { send(c, true); }
void hle_OSReceiveMessage(ppc::Ctx& c) {                                        // (queue, void** msg, flags)
    uint32_t q = c.r[3], out = c.r[4], flags = c.r[5];
    for (;;) {
        QueueState& s = g_queue[q];
        if (!s.msgs.empty()) {
            uint32_t m = s.msgs.front();
            s.msgs.pop_front();
            if (out) ppc::wr32(c, out, m);
            os_sched().wake_one(send_key(q));
            c.r[3] = 1;
            return;
        }
        if (!(flags & 1)) { c.r[3] = 0; return; }
        os_sched().block_on(recv_key(q));
    }
}
```

`runtime/hle/os_misc.cpp`:

```cpp
#include "hle/os.h"
#include "clock.h"
#include "fatal.h"
#include "guest_call.h"
#include <cstdio>
#include <map>

using namespace uc;
namespace {
struct Alarm { uint64_t gen = 0; };
std::map<uint32_t, Alarm> g_alarms;

void arm(uint32_t alarm, uint64_t at, uint64_t period, uint32_t handler, uint64_t gen) {
    os_sched().post_at(at, [=] {
        auto it = g_alarms.find(alarm);
        if (it == g_alarms.end() || it->second.gen != gen) return;   // cancelled or re-armed
        if (period) {   // absolute schedule: a late tick must not shift all later ticks
            uint64_t next = at + period, now = now_ticks();
            if (next <= now) next += ((now - next) / period + 1) * period;
            arm(alarm, next, period, handler, gen);
        }
        call_guest(os_sched().irq_ctx(), handler, {alarm, os_dummy_context()});
    });
}
}  // namespace

void hle_OSDisableInterrupts(ppc::Ctx& c) { c.r[3] = os_sched().set_interrupts(false) ? 1 : 0; }
void hle_OSEnableInterrupts(ppc::Ctx& c) { c.r[3] = os_sched().set_interrupts(true) ? 1 : 0; }
void hle_OSRestoreInterrupts(ppc::Ctx& c) { c.r[3] = os_sched().set_interrupts(c.r[3] != 0) ? 1 : 0; }

void hle_OSGetTime(ppc::Ctx& c) {
    uint64_t t = now_ticks();
    c.r[3] = uint32_t(t >> 32);
    c.r[4] = uint32_t(t);
    os_sched().safe_point();   // a guest that polls the time may be waiting for an event
}
void hle_OSGetTick(ppc::Ctx& c) {
    c.r[3] = uint32_t(now_ticks());
    os_sched().safe_point();
}

// OSSetAlarm(alarm, relative_ticks(r4:r5), handler(r6)); OSSetPeriodicAlarm(alarm, absolute_start(r4:r5), period(r6:r7), handler(r8))
void hle_OSSetAlarm(ppc::Ctx& c) {
    uint64_t rel = uint64_t(c.r[4]) << 32 | c.r[5];
    uint64_t gen = ++g_alarms[c.r[3]].gen;
    arm(c.r[3], now_ticks() + rel, 0, c.r[6], gen);
}
void hle_OSSetPeriodicAlarm(ppc::Ctx& c) {
    uint64_t start = uint64_t(c.r[4]) << 32 | c.r[5], period = uint64_t(c.r[6]) << 32 | c.r[7];
    if (!period) return;
    uint64_t gen = ++g_alarms[c.r[3]].gen;
    arm(c.r[3], start, period, c.r[8], gen);
}
void hle_OSCancelAlarm(ppc::Ctx& c) { ++g_alarms[c.r[3]].gen; }

void hle_OSReport(ppc::Ctx& c) {
    std::string s = format_guest(c, c.r[3], 4);
    std::fprintf(stderr, "[OSReport] %s", s.c_str());
    if (s.empty() || s.back() != '\n') std::fputc('\n', stderr);
}
void hle_OSPanic(ppc::Ctx& c) {   // (file, line, fmt, ...)
    std::string file; for (uint32_t a = c.r[3]; file.size() < 200; ++a) { char ch = char(ppc::rd8(c, a)); if (!ch) break; file.push_back(ch); }
    std::string msg = format_guest(c, c.r[5], 6);
    uc::fatal("OSPanic %s:%u: %s", file.c_str(), c.r[4], msg.c_str());
}
```

`runtime/hle/format_guest.cpp`:

```cpp
#include "hle/os.h"
#include <cstdio>
#include <cstring>

namespace uc {

std::string format_guest(ppc::Ctx& c, uint32_t fmt_addr, int first_gpr) {
    auto cstr = [&](uint32_t a) { std::string s; while (s.size() < 1024) { char ch = char(ppc::rd8(c, a++)); if (!ch) break; s.push_back(ch); } return s; };
    int gpr = first_gpr, fpr = 1, stack_i = 0;
    auto next_int = [&]() -> uint32_t { return gpr <= 10 ? c.r[gpr++] : ppc::rd32(c, c.r[1] + 8 + 4 * uint32_t(stack_i++)); };
    auto next_dbl = [&]() -> double {
        if (fpr <= 8) return c.f[fpr++];
        uint32_t a = c.r[1] + 8 + 4 * uint32_t(stack_i); stack_i += 2;
        return ppc::rdf64(c, a);
    };
    std::string fmt = cstr(fmt_addr), out;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
        size_t j = i + 1;
        while (j < fmt.size() && std::strchr("-+ #0123456789.", fmt[j])) ++j;
        while (j < fmt.size() && std::strchr("lhz", fmt[j])) ++j;   // length modifiers do not change the 32-bit guest ABI
        if (j >= fmt.size()) break;
        std::string spec = fmt.substr(i, j - i);
        std::string flags; for (char ch : spec) if (!std::strchr("lhz", ch)) flags.push_back(ch);
        char conv = fmt[j], buf[512];
        switch (conv) {
            case '%': out.push_back('%'); break;
            case 'd': case 'i': std::snprintf(buf, sizeof buf, (flags + "d").c_str(), int(next_int())); out += buf; break;
            case 'u': case 'x': case 'X': case 'o': case 'c': std::snprintf(buf, sizeof buf, (flags + conv).c_str(), next_int()); out += buf; break;
            case 'p': std::snprintf(buf, sizeof buf, "0x%08x", next_int()); out += buf; break;
            case 's': { std::string s = cstr(next_int()); std::snprintf(buf, sizeof buf, (flags + "s").c_str(), s.c_str()); out += buf; break; }
            case 'f': case 'g': case 'e': case 'G': case 'E': std::snprintf(buf, sizeof buf, (flags + conv).c_str(), next_dbl()); out += buf; break;
            default: out += spec + conv; break;
        }
        i = j;
    }
    return out;
}

}  // namespace uc
```

Add the four new `.cpp` files to `uc_runtime`. Append to `project/hle_map.txt` the SDK names implemented here (the names present in `identified_sdk.txt`/`symbols.txt`): `OSCreateThread OSResumeThread OSSuspendThread OSSleepThread OSWakeupThread OSYieldThread OSExitThread OSJoinThread OSSetThreadPriority OSInitMutex OSLockMutex OSUnlockMutex OSInitMessageQueue OSSendMessage OSReceiveMessage OSJamMessage OSDisableInterrupts OSEnableInterrupts OSRestoreInterrupts OSGetTime OSGetTick OSSetAlarm OSSetPeriodicAlarm OSCancelAlarm OSReport OSPanic`, plus `__OSGetSystemTime OSGetTime`. A name that Task 1 could not locate stays out of the file; the generator reports it.

- [ ] **Step 4: Run to verify**

Run: `scripts\dev.bat` — Expected: `test_os_hle` PASSED and all earlier tests green. Then `build.bat --no-install` — Expected: exit 0, `hle.cfg: N entries`; the game library no longer defines the replaced functions' bodies (they forward to `hle_*`), and linking `uc_smoke` fails with undefined `hle_*` symbols only if `uc_smoke` does not link `uc_runtime`: make `game/CMakeLists.txt` link `uc_runtime` (already PUBLIC via `uc_game`) and fix any missing HLE definition by adding it or removing its line from `hle_map.txt`.

---

### Task 6: DVD file system and HLE (TDD)

**Files:**

- Create: `runtime/dvdfs.h`, `runtime/dvdfs.cpp`, `runtime/hle/dvd.cpp`, `tests/test_dvd.cpp`
- Modify: `runtime/hle/os_layout.h` (DVD offsets from R0), `runtime/CMakeLists.txt`, `tests/CMakeLists.txt`, `project/hle_map.txt`

**Interfaces:**

- Consumes: `uc::Sched`, `uc::call_guest`, `uc::os_sched`, DVD layout offsets from `docs/spikes/R0-layouts.md`.
- Produces:
  - `class uc::DvdFs { bool open_root(const std::filesystem::path&); int entry_of(std::string path) const; bool is_dir(int) const; uint32_t length(int) const; int64_t read(int entry, void* dst, uint32_t len, uint32_t offset) const; size_t entry_count() const; std::string path_of(int) const; };`
  - `void uc::dvd_hle_init(DvdFs&)`; HLE: `hle_DVDInit hle_DVDConvertPathToEntrynum hle_DVDFastOpen hle_DVDOpen hle_DVDReadPrio hle_DVDReadAsyncPrio hle_DVDClose`.
  - Policy (taken from the SDK's own check in `DVDReadAsyncPrio`, see R0-layouts.md): a read is accepted when `offset <= length` and `offset + len < length + 32`; bytes past the end of the file are zero and the call returns the requested length. Anything else, or a directory, returns -1.

- [ ] **Step 1: Write the failing tests**

`tests/test_dvd.cpp`:

```cpp
#include "check.h"
#include "dispatch.h"
#include "dvdfs.h"
#include "ppc_fixture.h"
#include "sched.h"
#include "hle/os.h"
#include "hle/os_layout.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
namespace fs = std::filesystem;
using namespace ppc;

void hle_DVDConvertPathToEntrynum(Ctx&); void hle_DVDFastOpen(Ctx&); void hle_DVDOpen(Ctx&);
void hle_DVDReadPrio(Ctx&); void hle_DVDReadAsyncPrio(Ctx&);

static void write_file(const fs::path& p, size_t n) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    for (size_t i = 0; i < n; ++i) f.put(char(i * 7 + 3));
}
static uint8_t pat(size_t i) { return uint8_t(i * 7 + 3); }

static std::vector<std::string> g_log;
static void guest_cb(Ctx& c) { g_log.push_back("cb(" + std::to_string(int(c.r[3])) + ")"); uc::os_sched().wake_all(0x5000); }
static void put_cstr(Ctx& c, uint32_t a, const std::string& s) { for (size_t i = 0; i <= s.size(); ++i) wr8(c, a + uint32_t(i), uint32_t(i < s.size() ? s[i] : 0)); }

int main() {
    fs::path root = fs::temp_directory_path() / "uc_dvd_test";
    fs::remove_all(root);
    write_file(root / "Data" / "a.bin", 100);
    write_file(root / "Data" / "sub" / "b.bin", 64);
    write_file(root / "zero.bin", 0);

    uc::DvdFs d;
    CHECK(d.open_root(root));
    CHECK(!uc::DvdFs().open_root(root / "missing"));
    CHECK(d.entry_of("") == 0 && d.is_dir(0));
    int a = d.entry_of("Data/a.bin");
    CHECK(a > 0 && !d.is_dir(a) && d.length(a) == 100);
    CHECK(d.entry_of("/data/A.BIN") == a);              // leading slash, any case
    CHECK(d.entry_of("Data\\a.bin") == a);              // either separator
    CHECK(d.entry_of("Data/sub/b.bin") > 0 && d.is_dir(d.entry_of("Data/sub")));
    CHECK(d.entry_of("Data/missing.bin") == -1);
    CHECK(d.entry_of("../x") == -1 && d.entry_of("Data/../Data/a.bin") == -1);   // no way out of the root
    CHECK(d.entry_count() == 6);                         // root, Data, a.bin, sub, b.bin, zero.bin
    // reads
    uint8_t buf[256];
    CHECK(d.read(a, buf, 100, 0) == 100 && buf[0] == pat(0) && buf[99] == pat(99));
    CHECK(d.read(a, buf, 10, 50) == 10 && buf[0] == pat(50));
    CHECK(d.read(a, buf, 64, 80) == 20);                 // short read at the end of the file
    CHECK(d.read(a, buf, 8, 100) == 0 && d.read(a, buf, 8, 5000) == 0);
    CHECK(d.read(a, buf, 0, 10) == 0);
    CHECK(d.read(d.entry_of("zero.bin"), buf, 32, 0) == 0);
    CHECK(d.read(d.entry_of("Data"), buf, 4, 0) == -1);  // directory
    CHECK(d.read(999, buf, 4, 0) == -1);

    // HLE glue
    Fix f; Ctx& c = f.c;
    uc::Sched s(c.mem);
    static const uc::FnEntry table[] = {{0x80100000, guest_cb}};
    uc::dispatch_init(table, 1, 0x80100000, 0x80100004);
    uc::os_hle_init(s, c.mem, 0x817F0000);
    s.irq_ctx().r[1] = 0x817EFF00;
    uc::dvd_hle_init(d);
    uc::Thread* m = s.create(0, 16, [&](Ctx& t) {
        put_cstr(t, 0x80360000, "/Data/a.bin");
        t.r[3] = 0x80360000; hle_DVDConvertPathToEntrynum(t);
        CHECK(int(t.r[3]) == a);
        put_cstr(t, 0x80360100, "/nope");
        t.r[3] = 0x80360100; hle_DVDConvertPathToEntrynum(t);
        CHECK(int(t.r[3]) == -1);
        t.r[3] = 0x80360000; t.r[4] = 0x80361000; hle_DVDOpen(t);               // fileInfo at 0x80361000
        CHECK(t.r[3] == 1 && rd32(t, 0x80361000 + uc::layout::kFiLength) == 100);
        t.r[3] = 0x80360100; t.r[4] = 0x80361100; hle_DVDOpen(t);
        CHECK(t.r[3] == 0);
        // synchronous read (rounded request at the end of the file is zero padded)
        t.r[3] = 0x80361000; t.r[4] = 0x80362000; t.r[5] = 64; t.r[6] = 64; t.r[7] = 2; hle_DVDReadPrio(t);   // 64 + 64 = 128 < 100 + 32
        CHECK(t.r[3] == 64 && rd8(t, 0x80362000) == pat(64) && rd8(t, 0x80362000 + 35) == pat(99) && rd8(t, 0x80362000 + 36) == 0);
        t.r[3] = 0x80361000; t.r[4] = 0x80362000; t.r[5] = 96; t.r[6] = 64; t.r[7] = 2; hle_DVDReadPrio(t);   // 160 >= 132: the SDK rejects it
        CHECK(int(t.r[3]) == -1);
        t.r[3] = 0x80361000; t.r[4] = 0x80362000; t.r[5] = 32; t.r[6] = 128; t.r[7] = 2; hle_DVDReadPrio(t);
        CHECK(int(t.r[3]) == -1);                                                 // starts past the end of the file (128 > 100)
        // asynchronous read: completes while this thread is blocked; the callback runs in interrupt context
        for (int i = 0; i < 64; ++i) wr8(t, 0x80363000 + uint32_t(i), 0xEE);
        t.r[3] = 0x80361000; t.r[4] = 0x80363000; t.r[5] = 32; t.r[6] = 0; t.r[7] = 0x80100000; t.r[8] = 2;
        hle_DVDReadAsyncPrio(t);
        CHECK(t.r[3] == 1);
        s.block_on(0x5000);                                                       // the callback wakes us
        CHECK(g_log.size() == 1 && g_log[0] == "cb(32)");
        CHECK(rd8(t, 0x80363000) == pat(0) && rd8(t, 0x80363000 + 31) == pat(31) && rd8(t, 0x80363000 + 32) == 0xEE);
    });
    s.run(m);
    fs::remove_all(root);
    return 0;
}
```

Add `test_dvd` to the `test_support` loop. Run: `scripts\dev.bat` — Expected: FAIL (`dvdfs.h` not found).

- [ ] **Step 2: Implement `runtime/dvdfs.{h,cpp}`**

```cpp
// dvdfs.h
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace uc {
class DvdFs {
public:
    bool open_root(const std::filesystem::path& root);
    int entry_of(std::string path) const;                 // -1 if absent; 0 is the root directory
    bool is_dir(int e) const { return valid(e) && e_[size_t(e)].dir; }
    uint32_t length(int e) const { return valid(e) ? e_[size_t(e)].size : 0; }
    int64_t read(int entry, void* dst, uint32_t len, uint32_t offset) const;   // bytes read (short at EOF), -1 on a bad entry
    size_t entry_count() const { return e_.size(); }
    std::string path_of(int e) const { return valid(e) ? e_[size_t(e)].rel : std::string(); }
private:
    struct Entry { std::string rel; std::filesystem::path full; bool dir; uint32_t size; };
    bool valid(int e) const { return e >= 0 && size_t(e) < e_.size(); }
    void walk(const std::filesystem::path& dir, const std::string& rel);
    std::vector<Entry> e_;
    std::map<std::string, int> index_;   // lower-case relative path -> entry
};
void dvd_hle_init(DvdFs& fs);   // defined in hle/dvd.cpp
}
```

```cpp
// dvdfs.cpp
#include "dvdfs.h"
#include <algorithm>
#include <cctype>
#include <fstream>

namespace fs = std::filesystem;
namespace uc {

static std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

void DvdFs::walk(const fs::path& dir, const std::string& rel) {
    std::vector<fs::directory_entry> kids;
    for (const auto& k : fs::directory_iterator(dir)) kids.push_back(k);
    std::sort(kids.begin(), kids.end(), [](const auto& a, const auto& b) { return lower(a.path().filename().string()) < lower(b.path().filename().string()); });
    for (const auto& k : kids) {
        std::string name = k.path().filename().string();
        std::string r = rel.empty() ? name : rel + "/" + name;
        bool is_dir = k.is_directory();
        e_.push_back({r, k.path(), is_dir, is_dir ? 0u : uint32_t(k.file_size())});
        index_[lower(r)] = int(e_.size()) - 1;
        if (is_dir) walk(k.path(), r);   // depth first, like an FST
    }
}

bool DvdFs::open_root(const fs::path& root) {
    e_.clear(); index_.clear();
    if (!fs::is_directory(root)) return false;
    e_.push_back({"", root, true, 0});
    index_[""] = 0;
    walk(root, "");
    return true;
}

int DvdFs::entry_of(std::string path) const {
    for (char& c : path) if (c == '\\') c = '/';
    std::string norm;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string part = path.substr(i, j - i);
        if (part == "..") return -1;                       // never leave the root
        if (!part.empty() && part != ".") norm += (norm.empty() ? "" : "/") + part;
        i = j + 1;
    }
    auto it = index_.find(lower(norm));
    return it == index_.end() ? -1 : it->second;
}

int64_t DvdFs::read(int entry, void* dst, uint32_t len, uint32_t offset) const {
    if (!valid(entry) || e_[size_t(entry)].dir) return -1;
    const Entry& e = e_[size_t(entry)];
    if (len == 0 || offset >= e.size) return 0;
    uint32_t n = std::min<uint32_t>(len, e.size - offset);
    std::ifstream f(e.full, std::ios::binary);
    if (!f) return -1;
    f.seekg(offset);
    f.read(static_cast<char*>(dst), n);
    return int64_t(f.gcount());
}

}  // namespace uc
```

`runtime/hle/os_layout.h`: add, from R0 (confirm): `constexpr uint32_t kCbState = 0x0C; kFiStart = 0x30; kFiLength = 0x34; kFiCallback = 0x38;`.

`runtime/hle/dvd.cpp`:

```cpp
#include "dvdfs.h"
#include "fatal.h"
#include "guest_call.h"
#include "hle/os.h"
#include "hle/os_layout.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

using namespace uc;

namespace {
DvdFs* g_fs;
std::mutex g_m; std::condition_variable g_cv; std::deque<std::function<void()>> g_jobs; bool g_worker = false;

void enqueue(std::function<void()> job) {   // one worker keeps requests in order, like the drive
    std::lock_guard<std::mutex> lk(g_m);
    g_jobs.push_back(std::move(job));
    if (!g_worker) {
        g_worker = true;
        std::thread([] {
            for (;;) {
                std::function<void()> j;
                { std::unique_lock<std::mutex> lk(g_m); g_cv.wait_for(lk, std::chrono::seconds(1), [] { return !g_jobs.empty(); }); if (g_jobs.empty()) { g_worker = false; return; } j = std::move(g_jobs.front()); g_jobs.pop_front(); }
                j();
            }
        }).detach();
    }
    g_cv.notify_one();
}

std::string cstr(ppc::Ctx& c, uint32_t a) { std::string s; while (s.size() < 1024) { char ch = char(ppc::rd8(c, a++)); if (!ch) break; s.push_back(ch); } return s; }

bool guest_range_ok(uint32_t a, uint32_t len) {   // MEM1 or MEM2 only
    uint64_t end = uint64_t(a) + len;
    return (a >= 0x80000000u && end <= 0x81800000ull) || (a >= 0x90000000u && end <= 0x94000000ull);
}

// Reads len bytes at offset into dst; the tail past the end of the file (at most 31 bytes) is zero.
int64_t read_padded(int entry, std::vector<uint8_t>& dst, uint32_t len, uint32_t offset) {
    uint32_t length = g_fs->length(entry);
    if (g_fs->is_dir(entry) || offset > length || uint64_t(offset) + len >= uint64_t(length) + 32) return -1;   // the SDK's own bounds check
    dst.assign(len, 0);
    int64_t n = g_fs->read(entry, dst.data(), len, offset);
    return n < 0 ? -1 : int64_t(len);
}
}  // namespace

namespace uc { void dvd_hle_init(DvdFs& fs) { g_fs = &fs; } }

void hle_DVDInit(ppc::Ctx&) {}
void hle_DVDConvertPathToEntrynum(ppc::Ctx& c) { c.r[3] = uint32_t(g_fs->entry_of(cstr(c, c.r[3]))); }
static void open_entry(ppc::Ctx& c, int e, uint32_t fi) {
    if (e < 0 || g_fs->is_dir(e)) { c.r[3] = 0; return; }
    ppc::wr32(c, fi + layout::kFiStart, uint32_t(e));       // the disc offset is replaced by the entry number
    ppc::wr32(c, fi + layout::kFiLength, g_fs->length(e));
    ppc::wr32(c, fi + layout::kFiCallback, 0);
    ppc::wr32(c, fi + layout::kCbState, 0);                 // DVD_STATE_END
    c.r[3] = 1;
}
void hle_DVDFastOpen(ppc::Ctx& c) { open_entry(c, int(c.r[3]), c.r[4]); }
void hle_DVDOpen(ppc::Ctx& c) { open_entry(c, g_fs->entry_of(cstr(c, c.r[3])), c.r[4]); }
void hle_DVDClose(ppc::Ctx& c) { c.r[3] = 1; }

// DVDReadPrio(fileInfo, buf, len, offset, prio) -> bytes read or -1
void hle_DVDReadPrio(ppc::Ctx& c) {
    uint32_t fi = c.r[3], buf = c.r[4], len = c.r[5], off = c.r[6];
    int e = int(ppc::rd32(c, fi + layout::kFiStart));
    if (!guest_range_ok(buf, len)) fatal("DVDReadPrio into an invalid guest buffer %08X+%u", buf, len);
    std::vector<uint8_t> data;
    int64_t n = read_padded(e, data, len, off);
    if (n < 0) { c.r[3] = uint32_t(-1); return; }
    std::memcpy(ppc::hp(c, buf), data.data(), len);
    c.r[3] = len;
}

// DVDReadAsyncPrio(fileInfo, buf, len, offset, callback, prio) -> 1; callback(result, fileInfo) runs in interrupt context
void hle_DVDReadAsyncPrio(ppc::Ctx& c) {
    uint32_t fi = c.r[3], buf = c.r[4], len = c.r[5], off = c.r[6], cb = c.r[7];
    int e = int(ppc::rd32(c, fi + layout::kFiStart));
    if (!guest_range_ok(buf, len)) fatal("DVDReadAsyncPrio into an invalid guest buffer %08X+%u", buf, len);
    ppc::wr32(c, fi + layout::kCbState, 1);                 // DVD_STATE_BUSY
    Sched& s = os_sched();
    uint8_t* mem = c.mem;
    enqueue([=, &s] {
        auto data = std::make_shared<std::vector<uint8_t>>();
        int64_t n = read_padded(e, *data, len, off);
        s.post_external([=, &s] {                           // runs on the baton holder, with interrupts enabled
            ppc::Ctx& ic = s.irq_ctx();
            if (n >= 0) std::memcpy(mem + (buf & 0x3FFFFFFFu), data->data(), len);
            ppc::wr32(ic, fi + layout::kCbState, 0);
            if (cb) call_guest(ic, cb, {uint32_t(n < 0 ? -1 : int64_t(len)), fi});
        });
    });
    c.r[3] = 1;
}
```

Add `#include <cstring>`, `<memory>`, `<vector>`, `<functional>`, `<chrono>`. Add `dvdfs.cpp hle/dvd.cpp` to `uc_runtime`. Append to `project/hle_map.txt`: `DVDInit DVDConvertPathToEntrynum DVDFastOpen DVDOpen DVDReadPrio DVDReadAsyncPrio DVDClose` (and `DVDClose` only if Task 1 located it).

- [ ] **Step 3: Run to verify**

Run: `scripts\dev.bat` — Expected: `test_dvd` PASSED, everything else green. The async test proves a completion posted from the worker thread is delivered while every guest thread is blocked (Review Focus 6).

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
- Modify: `game/CMakeLists.txt`, `build.bat`, `runtime/dvdfs.h`, `runtime/hle/dvd.cpp`, `.gitignore` (add `build-trace/`, `build-trace.log`)

**Interfaces:**

- Consumes: every runtime piece from Tasks 2–7, the generated `uc_functions` table, `project/identified_sdk.txt`, `project/symbols.txt`.
- Produces:
  - `uc.exe --game <dir> [--seconds N] [--log-dvd]` (default game dir `S:\Games\Umbrella Chronicles\game`); exit code 0 normal, 3 guest trap or fatal runtime error, 4 no-progress watchdog.
  - `uc::DvdStats { opens, reads, bytes }`, `uc::dvd_stats()`, `uc::dvd_log(bool)`; `uc::trace_dump_all(FILE*, int)`.
  - `scripts\translate.bat` (hle.cfg generation + cached translation, shared by `build.bat` and `bringup.bat`); `scripts\bringup.bat [uc args]` (trace build in `build-trace\`, runs `uc.exe`, prints the log tail).

- [ ] **Step 1: Counters and logging for the stats line**

`runtime/dvdfs.h`: add to the namespace

```cpp
struct DvdStats { uint64_t opens = 0, reads = 0, bytes = 0; };
DvdStats dvd_stats();
void dvd_log(bool on);   // print "[dvd] open <path>" lines to stderr
```

`runtime/hle/dvd.cpp`: add `DvdStats g_stats; bool g_log = false;` (atomics not needed: only the baton holder touches them), define `dvd_stats()` and `dvd_log()`, increment `g_stats.opens` and log the path in `open_entry` after a successful open (`std::fprintf(stderr, "[dvd] open %s\n", g_fs->path_of(e).c_str());` when `g_log`), and increment `reads`/`bytes` in `hle_DVDReadPrio` and the async completion.

(`runtime/trace.h` and `trace.cpp` already provide `trace_dump_all` from Task 2.)

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
    fs::path game = "S:\\Games\\Umbrella Chronicles\\game";
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
if not defined UC_INSTALL_DIR set "UC_INSTALL_DIR=S:\Games\Umbrella Chronicles"
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
| a device would need more than a read-back register file (status bits that change, mailboxes, DMA, interrupts) | translated SDK code is driving hardware | **do not model the device**: HLE the SDK function(s) that drive it at call level (`xref --callers`, or the named function in `identified_sdk.txt`) and note in `tech-debt.md` which device model was avoided; only if no SDK function can be identified, write the smallest model and record it as debt |
| `[OSReport]` text                                                                                                                       | the game explains itself                          | read it first; it often names the missing piece                                                                                                                                                                                                                 |

3. If the change is more than a config line, `Nop` or a `RegFile` (that is, if it contains logic), write the unit test first (RED), then the code (GREEN), then `scripts\dev.bat`.
4. Append to `R1-bringup-log.md`: iteration number, the exact symptom line, the function (with `xref` evidence), the change, and the new first symptom.
5. **Nativity check for every change.** Ask: does this add a hardware model, a handshake answer, or a `Nop`/`Return*` that discards behaviour the game could see? Then (a) prefer a native HLE of the SDK function at call level over any model beyond a plain `RegFile`, and (b) add an entry to `docs/tech-debt.md` in the same step (ID `TD-nnn`, what it imitates, why it was needed, the native replacement, when to fix it). A pure `RegFile` for a block the game only pokes, and a `Nop` for CPU-setup code (caches, BATs, exception vectors) that has no observable effect on a native build, are not debt; say so in the log line.

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

Review `docs/tech-debt.md`: every entry has a native replacement and a trigger; list the entries added by this plan in the hand-off. At the top of `R1-bringup-log.md` write: iterations used, final `stats:` line, the first ten `[dvd] open` paths, the list of `Nop`/`Return*` stubs added (each with the reason), devices added, struct offsets that were corrected, and what M1 does **not** cover (no GX output, no audio, no movies, no input).

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
