# Foundation and Spikes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Working toolchain, a C++ project skeleton with a one-click `build.bat` (build → `dist\` → copy to `S:\Games\Umbrella Chronicles`), a tested DOL parser/scanner, and go/no-go answers for spikes S1, S3, S4.

**Architecture:** CMake + Ninja + clang-cl. `common/` holds the DOL parser and scanner (first pieces of the future analyzer). `tools/s1_scan` is a CLI over them. Spikes S3 and S4 are throwaway research artifacts under `tools/spikes/` and `docs/spikes/`, kept out of the main build.

**Tech Stack:** C++20, CMake ≥ 3.25, Ninja, clang-cl (LLVM) on MSVC Build Tools, Vulkan SDK (includes `slangc`), Python 3 (spike scripts only), Windows batch.

**Spec:** `docs/superpowers/specs/2026-10-12-uc-recomp-design.md` (§10 Build, §11 Spikes)

## Global Constraints

- Native Windows x64 only; no emulator, interpreter, JIT or PowerPC code in any runtime artifact.
- Generated/native code is compiled with clang-cl, `/O2`, `x86-64-v3`.
- Game input: extracted Wii game at `S:\Games\Umbrella Chronicles\game`; executable at `DATA\sys\main.dol`.
- Build output goes to `<project>\dist`, then `robocopy` to `S:\Games\Umbrella Chronicles` (the folder root, next to `game\` and `extractor\`).
- `build.bat` is run manually and exits non-zero on any failure.
- Project root: `D:\Projects\Native Ports\Umbrella-chronicles-recomp` (path contains spaces; quote everything).
- Commit steps are omitted on purpose: the user asked that commits happen only on request. Do not run `git commit`.
- Spike code is throwaway and must be labelled as such.

## Review Focus

1. Truncated or garbage DOL (shorter than header, section past EOF, all-zero header) → parser returns an error, never crashes (Task 2 tests).
2. DOL section whose size is not a multiple of 4 → scanner ignores the tail, no out-of-bounds read (Task 3 test).
3. `bl` whose target lies outside every text section → not counted as a function (Task 3 test).
4. Install directory or DOL missing → `build.bat` prints a clear error and exits non-zero (Task 4 steps).
5. `build.bat` run twice, and from another working directory → second run succeeds; robocopy exit codes 1–7 are success (Task 4 steps).

## File Structure

```
.gitignore
CMakeLists.txt                  root, includes subdirs
scripts/env.bat                 sets up MSVC + LLVM + Vulkan SDK env; exit 1 if a tool is missing
scripts/dev.bat                 env + configure + build + ctest (used by TDD steps and build.bat)
build.bat                       one-click build (spec §10)
common/CMakeLists.txt           uc_common static lib
common/dol.h, dol.cpp           DOL header parser
common/dol_scan.h, dol_scan.cpp function-count heuristics, zero-run check, SDK banner strings
tools/s1_scan/CMakeLists.txt
tools/s1_scan/main.cpp          S1 CLI
tests/CMakeLists.txt
tests/check.h                   CHECK macro
tests/dol_builder.h             synthetic DOL builder
tests/test_dol.cpp
tests/test_dol_scan.cpp
tools/spikes/s4_gen.py          S4 synthetic translated-code generator (throwaway)
tools/spikes/s4/CMakeLists.txt  S4 compile-time benchmark project (throwaway)
docs/spikes/S1-sdk-scan.md      results
docs/spikes/S3-fg-vulkan.md     results
docs/spikes/S4-compile-time.md  results
```

---

### Task 1: Install and verify the toolchain

**Files:**
- Create (scratch only, not in project): `<scratchpad>\hello.slang`

**Interfaces:**
- Produces: `clang-cl`, `cmake`, `ninja`, `slangc` reachable after `scripts\env.bat` (Task 2); `%VULKAN_SDK%` set machine-wide.

- [ ] **Step 1: Install MSVC Build Tools (UAC prompt appears; accept it)**

```
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

- [ ] **Step 2: Install LLVM and the Vulkan SDK**

```
winget install --id LLVM.LLVM -e
winget install --id KhronosGroup.VulkanSDK -e
```

If a winget id is not found, run `winget search LLVM` / `winget search Vulkan` and pick the official package; record the id used in `docs/spikes/S4-compile-time.md` later.

- [ ] **Step 3: Verify in a NEW terminal (env vars are machine-level)**

```
"%ProgramFiles%\LLVM\bin\clang-cl.exe" --version
echo %VULKAN_SDK%
"%VULKAN_SDK%\Bin\slangc.exe" -version
vulkaninfo --summary
```

Expected: clang-cl 17 or newer; `VULKAN_SDK` prints a path; `slangc` prints a version; `vulkaninfo` lists the RTX 4080 with Vulkan 1.3 or newer.

If `slangc.exe` is absent from the Vulkan SDK `Bin`, install Slang from its GitHub releases into `C:\Tools\slang` and add that `bin` directory to the user PATH.

- [ ] **Step 4: Verify Slang → SPIR-V**

Write `hello.slang` in the scratchpad:

```slang
[shader("compute")]
[numthreads(1, 1, 1)]
void main() {}
```

Run: `"%VULKAN_SDK%\Bin\slangc.exe" hello.slang -target spirv -entry main -stage compute -o hello.spv`
Expected: exit 0 and `hello.spv` exists.

---

### Task 2: Project skeleton and DOL parser (TDD)

**Files:**
- Create: `.gitignore`, `CMakeLists.txt`, `scripts/env.bat`, `scripts/dev.bat`, `common/CMakeLists.txt`, `common/dol.h`, `common/dol.cpp`, `tests/CMakeLists.txt`, `tests/check.h`, `tests/dol_builder.h`, `tests/test_dol.cpp`

**Interfaces:**
- Produces (`namespace uc`, `common/dol.h`):
  - `struct DolSection { uint32_t file_offset; uint32_t address; uint32_t size; bool is_text; };`
  - `struct DolImage { std::vector<DolSection> sections; uint32_t bss_address = 0, bss_size = 0, entry = 0; };`
  - `struct DolResult { bool ok = false; std::string error; DolImage image; };`
  - `DolResult parse_dol(std::span<const uint8_t> file);`
  - `std::span<const uint8_t> section_bytes(std::span<const uint8_t> file, const DolSection& s);`
- Produces (`tests/dol_builder.h`): `put32(std::vector<uint8_t>&, size_t off, uint32_t)`, `make_dol(const std::vector<uint32_t>& code, const std::vector<uint8_t>& data = {}) -> std::vector<uint8_t>` (text section at `0x80003100`, data section at `0x80400000` when `data` is non-empty, entry = `0x80003100`).
- Produces: `scripts\dev.bat [ctest args]` configures, builds, runs tests; exit 0 only if all pass.

- [ ] **Step 1: Write scaffolding (no product code yet)**

`.gitignore`:
```
build/
dist/
generated/
*.spv
```

`scripts/env.bat` (does not use `setlocal`, so callers keep the environment):
```bat
@echo off
rem Sets up MSVC x64 + LLVM + Vulkan SDK. Returns 1 if anything is missing.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :no_vs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if exist "%ProgramFiles%\LLVM\bin\clang-cl.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
where clang-cl >nul 2>nul || (echo ERROR: clang-cl not found. Install LLVM, see plan Task 1. & exit /b 1)
where cmake >nul 2>nul || (echo ERROR: cmake not found. & exit /b 1)
where ninja >nul 2>nul || (echo ERROR: ninja not found. & exit /b 1)
if not defined VULKAN_SDK (echo ERROR: VULKAN_SDK is not set. Install the Vulkan SDK, see plan Task 1. & exit /b 1)
if not exist "%VULKAN_SDK%\Bin\slangc.exe" (echo ERROR: slangc.exe not found in the Vulkan SDK. & exit /b 1)
exit /b 0
:no_vs
echo ERROR: Visual Studio Build Tools with the C++ workload were not found. See plan Task 1.
exit /b 1
```

`scripts/dev.bat`:
```bat
@echo off
setlocal
set "ROOT=%~dp0.."
call "%~dp0env.bat" || exit /b 1
cmake -S "%ROOT%" -B "%ROOT%\build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
cmake --build "%ROOT%\build" || exit /b 1
ctest --test-dir "%ROOT%\build" --output-on-failure %* || exit /b 1
exit /b 0
```

`CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.25)
project(uc LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_compile_options(/clang:-march=x86-64-v3)
endif()
enable_testing()
add_subdirectory(common)
add_subdirectory(tests)
```

`common/CMakeLists.txt`:
```cmake
add_library(uc_common STATIC dol.cpp)
target_include_directories(uc_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
```

`tests/CMakeLists.txt`:
```cmake
foreach(t test_dol)
  add_executable(${t} ${t}.cpp)
  target_link_libraries(${t} PRIVATE uc_common)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${t} COMMAND ${t})
endforeach()
```

`tests/check.h`:
```cpp
#pragma once
#include <cstdio>
#include <cstdlib>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
```

`tests/dol_builder.h`:
```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

inline void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    v[off] = uint8_t(x >> 24); v[off + 1] = uint8_t(x >> 16);
    v[off + 2] = uint8_t(x >> 8); v[off + 3] = uint8_t(x);
}

// Text section 0 at 0x80003100 holds `code`; optional data section 0 at 0x80400000 holds `data`.
inline std::vector<uint8_t> make_dol(const std::vector<uint32_t>& code, const std::vector<uint8_t>& data = {}) {
    const uint32_t text_addr = 0x80003100, data_addr = 0x80400000;
    std::vector<uint8_t> v(0x100 + code.size() * 4 + data.size(), 0);
    put32(v, 0x00, 0x100);
    put32(v, 0x48, text_addr);
    put32(v, 0x90, uint32_t(code.size() * 4));
    put32(v, 0xE0, text_addr);
    for (size_t i = 0; i < code.size(); ++i) put32(v, 0x100 + i * 4, code[i]);
    if (!data.empty()) {
        put32(v, 0x1C, uint32_t(0x100 + code.size() * 4));
        put32(v, 0x64, data_addr);
        put32(v, 0xAC, uint32_t(data.size()));
        for (size_t i = 0; i < data.size(); ++i) v[0x100 + code.size() * 4 + i] = data[i];
    }
    return v;
}
```

- [ ] **Step 2: Write the failing test**

`tests/test_dol.cpp`:
```cpp
#include "check.h"
#include "dol.h"
#include "dol_builder.h"

int main() {
    // valid: one text section, entry, address, flags
    {
        auto f = make_dol({0x60000000, 0x4E800020});
        auto r = uc::parse_dol(f);
        CHECK(r.ok);
        CHECK(r.image.sections.size() == 1);
        CHECK(r.image.sections[0].is_text);
        CHECK(r.image.sections[0].address == 0x80003100);
        CHECK(r.image.sections[0].size == 8);
        CHECK(r.image.entry == 0x80003100);
        CHECK(uc::section_bytes(f, r.image.sections[0]).size() == 8);
    }
    // text + data
    {
        auto f = make_dol({0x60000000}, {1, 2, 3, 4, 5, 6, 7, 8});
        auto r = uc::parse_dol(f);
        CHECK(r.ok);
        CHECK(r.image.sections.size() == 2);
        CHECK(!r.image.sections[1].is_text);
        CHECK(r.image.sections[1].address == 0x80400000);
    }
    // review focus 1: truncated / garbage input must fail cleanly
    {
        std::vector<uint8_t> tiny(0x40, 0);
        CHECK(!uc::parse_dol(tiny).ok);
        CHECK(!uc::parse_dol({}).ok);
        std::vector<uint8_t> zero_header(0x100, 0);
        CHECK(!uc::parse_dol(zero_header).ok);  // no sections
        auto f = make_dol({0x60000000, 0x60000000});
        put32(f, 0x90, 0x1000);                  // size runs past EOF
        CHECK(!uc::parse_dol(f).ok);
        auto g = make_dol({0x60000000});
        put32(g, 0x48, 0x00001000);              // address below 0x80000000
        CHECK(!uc::parse_dol(g).ok);
    }
    return 0;
}
```

- [ ] **Step 3: Run it to verify it fails**

Run: `scripts\dev.bat`
Expected: build FAILS (`dol.h` / `dol.cpp` not found).

- [ ] **Step 4: Write the implementation**

`common/dol.h`:
```cpp
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace uc {

struct DolSection {
    uint32_t file_offset;
    uint32_t address;
    uint32_t size;
    bool is_text;
};

struct DolImage {
    std::vector<DolSection> sections;  // non-empty sections only, text first
    uint32_t bss_address = 0, bss_size = 0, entry = 0;
};

struct DolResult {
    bool ok = false;
    std::string error;
    DolImage image;
};

DolResult parse_dol(std::span<const uint8_t> file);
std::span<const uint8_t> section_bytes(std::span<const uint8_t> file, const DolSection& s);

}  // namespace uc
```

`common/dol.cpp`:
```cpp
#include "dol.h"

namespace uc {

static uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

DolResult parse_dol(std::span<const uint8_t> f) {
    DolResult r;
    if (f.size() < 0x100) { r.error = "file is smaller than the 0x100-byte DOL header"; return r; }
    for (int i = 0; i < 18; ++i) {  // 7 text + 11 data
        uint32_t off = be32(&f[i * 4]);
        uint32_t addr = be32(&f[0x48 + i * 4]);
        uint32_t size = be32(&f[0x90 + i * 4]);
        if (size == 0) continue;
        if (uint64_t(off) + size > f.size()) {
            r.error = "section " + std::to_string(i) + " extends past end of file";
            return r;
        }
        if (addr < 0x80000000u) {
            r.error = "section " + std::to_string(i) + " address is below 0x80000000";
            return r;
        }
        r.image.sections.push_back({off, addr, size, i < 7});
    }
    if (r.image.sections.empty()) { r.error = "DOL has no non-empty sections"; return r; }
    r.image.bss_address = be32(&f[0xD8]);
    r.image.bss_size = be32(&f[0xDC]);
    r.image.entry = be32(&f[0xE0]);
    r.ok = true;
    return r;
}

std::span<const uint8_t> section_bytes(std::span<const uint8_t> file, const DolSection& s) {
    return file.subspan(s.file_offset, s.size);
}

}  // namespace uc
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `scripts\dev.bat`
Expected: configure/build succeed, `test_dol` PASSED, exit 0.

---

### Task 3: DOL scanner and the S1 CLI (TDD)

**Files:**
- Create: `common/dol_scan.h`, `common/dol_scan.cpp`, `tests/test_dol_scan.cpp`, `tools/s1_scan/CMakeLists.txt`, `tools/s1_scan/main.cpp`
- Modify: `common/CMakeLists.txt`, `tests/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `uc::parse_dol`, `uc::section_bytes`, `DolImage`, `DolSection` (Task 2); `make_dol`, `put32` (tests).
- Produces (`common/dol_scan.h`):
  - `constexpr size_t kSuspiciousZeroRunWords = 256;`
  - `struct ScanReport { size_t text_bytes = 0, stwu_prologues = 0, blr_count = 0, unique_bl_targets = 0, largest_zero_run_words = 0, suspicious_zero_runs = 0; };`
  - `struct SdkString { uint32_t address; std::string text; };`
  - `ScanReport scan_text(std::span<const uint8_t> file, const DolImage& image);`
  - `std::vector<SdkString> find_sdk_strings(std::span<const uint8_t> file, const DolImage& image);`
- Produces: executable `s1_scan <main.dol>` (exit 0 ok, 1 invalid/unreadable, 2 usage).

- [ ] **Step 1: Write the failing test**

`tests/test_dol_scan.cpp`:
```cpp
#include "check.h"
#include "dol.h"
#include "dol_builder.h"
#include "dol_scan.h"
#include <string>

static uc::ScanReport scan(const std::vector<uint8_t>& f) {
    auto d = uc::parse_dol(f);
    CHECK(d.ok);
    return uc::scan_text(f, d.image);
}

int main() {
    // bl forward (+8 lands on the 3rd word), blr x2
    {
        auto r = scan(make_dol({0x48000009, 0x60000000, 0x4E800020, 0x4E800020}));
        CHECK(r.unique_bl_targets == 1);
        CHECK(r.blr_count == 2);
        CHECK(r.text_bytes == 16);
    }
    // bl backward (-4 from word 1 lands on word 0); duplicates count once
    {
        auto r = scan(make_dol({0x60000000, 0x4BFFFFFD, 0x4BFFFFF9}));  // -4 and -8 -> targets word0, word0
        CHECK(r.unique_bl_targets == 1);
    }
    // review focus 3: bl target outside every text section is not a function
    {
        auto r = scan(make_dol({0x48001001, 0x4E800020}));  // bl +0x1000
        CHECK(r.unique_bl_targets == 0);
    }
    // stwu r1,-16(r1) prologue
    {
        auto r = scan(make_dol({0x9421FFF0, 0x7C0802A6, 0x4E800020}));
        CHECK(r.stwu_prologues == 1);
    }
    // zero runs: 300 zero words are suspicious; 10 are not
    {
        std::vector<uint32_t> code(300, 0);
        code.insert(code.begin(), 0x60000000);
        code.push_back(0x4E800020);
        auto r = scan(make_dol(code));
        CHECK(r.largest_zero_run_words == 300);
        CHECK(r.suspicious_zero_runs == 1);
        std::vector<uint32_t> small(10, 0);
        small.push_back(0x4E800020);
        auto s = scan(make_dol(small));
        CHECK(s.largest_zero_run_words == 10);
        CHECK(s.suspicious_zero_runs == 0);
    }
    // review focus 2: section size not a multiple of 4 -> tail ignored, no OOB read
    {
        auto f = make_dol({0x4E800020, 0x4E800020});
        put32(f, 0x90, 6);
        auto r = scan(f);
        CHECK(r.blr_count == 1);
    }
    // SDK banner strings are found in data sections
    {
        std::string banner = "<< RVL_SDK - GX \trelease build: Jul  1 2008>>";
        std::vector<uint8_t> data(banner.begin(), banner.end());
        data.push_back(0);
        auto f = make_dol({0x60000000}, data);
        auto d = uc::parse_dol(f);
        CHECK(d.ok);
        auto s = uc::find_sdk_strings(f, d.image);
        CHECK(s.size() == 1);
        CHECK(s[0].address == 0x80400000);
        CHECK(s[0].text.find("RVL_SDK") != std::string::npos);
    }
    return 0;
}
```

- [ ] **Step 2: Wire the test and run it to verify it fails**

`tests/CMakeLists.txt`: change `foreach(t test_dol)` to `foreach(t test_dol test_dol_scan)`.
Run: `scripts\dev.bat`
Expected: build FAILS (`dol_scan.h` not found).

- [ ] **Step 3: Write the implementation**

`common/dol_scan.h`:
```cpp
#pragma once
#include "dol.h"
#include <cstddef>
#include <string>
#include <vector>

namespace uc {

constexpr size_t kSuspiciousZeroRunWords = 256;

struct ScanReport {
    size_t text_bytes = 0;
    size_t stwu_prologues = 0;        // stwu r1,d(r1)
    size_t blr_count = 0;
    size_t unique_bl_targets = 0;     // bl targets inside text sections = function-count estimate
    size_t largest_zero_run_words = 0;
    size_t suspicious_zero_runs = 0;  // runs >= kSuspiciousZeroRunWords (possible runtime-filled code)
};

struct SdkString {
    uint32_t address;
    std::string text;
};

ScanReport scan_text(std::span<const uint8_t> file, const DolImage& image);
std::vector<SdkString> find_sdk_strings(std::span<const uint8_t> file, const DolImage& image);

}  // namespace uc
```

`common/dol_scan.cpp`:
```cpp
#include "dol_scan.h"
#include <unordered_set>

namespace uc {

static uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

ScanReport scan_text(std::span<const uint8_t> file, const DolImage& image) {
    ScanReport r;
    std::unordered_set<uint32_t> targets;
    for (const auto& s : image.sections) {
        if (!s.is_text) continue;
        auto b = section_bytes(file, s);
        size_t words = b.size() / 4;  // trailing partial word is ignored
        r.text_bytes += b.size();
        size_t run = 0;
        auto close_run = [&] {
            if (run > r.largest_zero_run_words) r.largest_zero_run_words = run;
            if (run >= kSuspiciousZeroRunWords) ++r.suspicious_zero_runs;
            run = 0;
        };
        for (size_t i = 0; i < words; ++i) {
            uint32_t w = be32(&b[i * 4]);
            uint32_t pc = s.address + uint32_t(i * 4);
            if (w == 0) { ++run; continue; }
            close_run();
            if ((w & 0xFFFF0000u) == 0x94210000u) ++r.stwu_prologues;
            if (w == 0x4E800020u) ++r.blr_count;
            if ((w >> 26) == 18 && (w & 3) == 1) {  // bl: AA=0, LK=1
                uint32_t li = w & 0x03FFFFFCu;
                if (li & 0x02000000u) li |= 0xFC000000u;
                targets.insert(pc + li);
            }
        }
        close_run();
    }
    for (uint32_t t : targets) {
        for (const auto& s : image.sections) {
            if (s.is_text && t >= s.address && t < s.address + (s.size & ~3u)) { ++r.unique_bl_targets; break; }
        }
    }
    return r;
}

std::vector<SdkString> find_sdk_strings(std::span<const uint8_t> file, const DolImage& image) {
    std::vector<SdkString> out;
    for (const auto& s : image.sections) {
        auto b = section_bytes(file, s);
        size_t start = 0;
        auto flush = [&](size_t end) {
            if (end - start < 8) return;
            std::string t(reinterpret_cast<const char*>(b.data()) + start, end - start);
            if (t.find("SDK") != std::string::npos) out.push_back({s.address + uint32_t(start), t});
        };
        for (size_t i = 0; i <= b.size(); ++i) {
            bool printable = i < b.size() && ((b[i] >= 0x20 && b[i] < 0x7F) || b[i] == '\t');
            if (!printable) { flush(i); start = i + 1; }
        }
    }
    return out;
}

}  // namespace uc
```

`common/CMakeLists.txt`: `add_library(uc_common STATIC dol.cpp dol_scan.cpp)` (keep the include-directories line).

- [ ] **Step 4: Add the S1 CLI**

`tools/s1_scan/CMakeLists.txt`:
```cmake
add_executable(s1_scan main.cpp)
target_link_libraries(s1_scan PRIVATE uc_common)
install(TARGETS s1_scan RUNTIME DESTINATION tools)
```

Root `CMakeLists.txt`: add `add_subdirectory(tools/s1_scan)` after `add_subdirectory(common)`.

`tools/s1_scan/main.cpp`:
```cpp
#include "dol.h"
#include "dol_scan.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: s1_scan <main.dol>\n"); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto dol = uc::parse_dol(file);
    if (!dol.ok) { std::fprintf(stderr, "invalid DOL: %s\n", dol.error.c_str()); return 1; }

    std::printf("entry %08X  bss %08X size %08X\n", dol.image.entry, dol.image.bss_address, dol.image.bss_size);
    for (const auto& s : dol.image.sections)
        std::printf("%s addr %08X size %08X off %08X\n", s.is_text ? "text" : "data", s.address, s.size, s.file_offset);

    auto r = uc::scan_text(file, dol.image);
    std::printf("text bytes            %zu\n", r.text_bytes);
    std::printf("stwu r1 prologues     %zu\n", r.stwu_prologues);
    std::printf("blr                   %zu\n", r.blr_count);
    std::printf("unique bl targets     %zu\n", r.unique_bl_targets);
    std::printf("largest zero run (w)  %zu\n", r.largest_zero_run_words);
    std::printf("suspicious zero runs  %zu\n", r.suspicious_zero_runs);

    auto strs = uc::find_sdk_strings(file, dol.image);
    std::printf("SDK strings: %zu\n", strs.size());
    for (size_t i = 0; i < strs.size() && i < 300; ++i) std::printf("%08X  %s\n", strs[i].address, strs[i].text.c_str());
    return 0;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `scripts\dev.bat`
Expected: `test_dol` and `test_dol_scan` PASSED; `build\tools\s1_scan\s1_scan.exe` exists.

---

### Task 4: `build.bat`

**Files:**
- Create: `build.bat`

**Interfaces:**
- Consumes: `scripts\dev.bat` (Task 2), `install(TARGETS s1_scan ...)` (Task 3).
- Produces: `build.bat [--no-install]`. Env override `UC_INSTALL_DIR` (default `S:\Games\Umbrella Chronicles`). Exit 0 on success, non-zero on any failure.

- [ ] **Step 1: Write `build.bat`**

```bat
@echo off
setlocal EnableExtensions
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
set "DIST=%ROOT%\dist"
if not defined UC_INSTALL_DIR set "UC_INSTALL_DIR=S:\Games\Umbrella Chronicles"
set "GAME_DOL=%UC_INSTALL_DIR%\game\DATA\sys\main.dol"
set "NO_INSTALL="
if /I "%~1"=="--no-install" set "NO_INSTALL=1"

echo [1/5] Checking game files
if not exist "%UC_INSTALL_DIR%\" (echo ERROR: install directory not found: %UC_INSTALL_DIR% & exit /b 1)
if not exist "%GAME_DOL%" (echo ERROR: main.dol not found: %GAME_DOL% & exit /b 1)

echo [2/5] Configure, build, test
call "%ROOT%\scripts\dev.bat" || (echo ERROR: build or tests failed & exit /b 1)

echo [3/5] Inspecting game executable
"%BUILD%\tools\s1_scan\s1_scan.exe" "%GAME_DOL%" > "%BUILD%\s1_report.txt" || (echo ERROR: s1_scan failed & exit /b 1)

echo [4/5] Staging dist
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%" || exit /b 1
cmake --install "%BUILD%" --prefix "%DIST%" || (echo ERROR: install to dist failed & exit /b 1)

if defined NO_INSTALL (echo [5/5] Skipping copy ^(--no-install^) & exit /b 0)

echo [5/5] Copying dist to %UC_INSTALL_DIR%
robocopy "%DIST%" "%UC_INSTALL_DIR%" /E /NFL /NDL /NJH /NJS /NP
if errorlevel 8 (echo ERROR: robocopy failed & exit /b 1)
echo Done.
exit /b 0
```

- [ ] **Step 2: Happy path from another working directory**

Run: `cd /d C:\ && "D:\Projects\Native Ports\Umbrella-chronicles-recomp\build.bat"`
Expected: exit 0; `dist\tools\s1_scan.exe` exists; `S:\Games\Umbrella Chronicles\tools\s1_scan.exe` exists; `build\s1_report.txt` has `entry` and `unique bl targets` lines.

- [ ] **Step 3: Second run succeeds (robocopy returns 0 or 1)**

Run the same command again. Expected: exit 0 (`echo %errorlevel%` prints 0).

- [ ] **Step 4: Missing install dir and missing DOL fail clearly**

```
set UC_INSTALL_DIR=S:\does-not-exist
build.bat
echo %errorlevel%
```
Expected: message `install directory not found`, errorlevel 1.

Then: `set UC_INSTALL_DIR=%TEMP%` and run `build.bat`. Expected: message `main.dol not found`, errorlevel 1. Clear the variable afterwards (`set UC_INSTALL_DIR=`).

---

### Task 5: Spike S1 — what the DOL gives us

**Files:**
- Create: `docs/spikes/S1-sdk-scan.md`

**Interfaces:**
- Consumes: `build\s1_report.txt` from Task 4.
- Produces: the unique-bl-target count N used by Task 7, and the go/no-go answer for SDK identification.

- [ ] **Step 1: Read the real report**

Run: `build\tools\s1_scan\s1_scan.exe "S:\Games\Umbrella Chronicles\game\DATA\sys\main.dol"`
Collect: section table, `unique bl targets`, `suspicious zero runs`, and the SDK string list.

- [ ] **Step 2: Classify the SDK strings**

From the strings, list which Nintendo libraries are linked (look for `OS`, `GX`, `DVD`, `VI`, `PAD`/`KPAD`/`WPAD`, `AX`, `NAND`, `MEM`, `THP`) with their build dates. A library absent from the strings is "unknown", not "absent".

- [ ] **Step 3: Write `docs/spikes/S1-sdk-scan.md`**

Record: raw numbers from the report, the library inventory table, and a verdict using these rules:
- PASS if the core libs (OS, GX, DVD, VI, KPAD/WPAD, AX) all appear and `suspicious zero runs` is 0. Next plan builds a mask-hash signature matcher for these libs.
- PARTIAL if some core libs are missing from strings. Next plan adds manual identification via call-graph and string cross-references for the missing ones.
- FAIL if `suspicious zero runs` > 0 (possible runtime-generated code). Next plan must investigate those ranges before translation.

**Open decision: Ghidra (and decomp-toolkit).** Not a build dependency; it would be a manual research tool only, with results stored as data in `project/uc.yml`.
- PASS: skip Ghidra. Identification is done by signatures alone.
- PARTIAL or FAIL: ask the user, then install Ghidra + JDK + a Gekko/Broadway loader plugin (verify current plugin names and DOL support first) and add a "manual identification" step to the analyzer plan. Cross-check function boundaries with decomp-toolkit (`dtk`) as a cheaper first step.
Record the decision and its reason in `docs/spikes/S1-sdk-scan.md`.

---

### Task 6: Spike S3 — DLSS and FSR frame generation on Vulkan

**Files:**
- Create: `docs/spikes/S3-fg-vulkan.md`

**Interfaces:**
- Produces: documented yes/no for DLSS-FG on Vulkan, the Streamline version, required Vulkan extensions, and the FSR frame generation Vulkan status.

- [ ] **Step 1: Fetch the SDK docs into the scratchpad (outside the project)**

```
git clone --depth 1 https://github.com/NVIDIAGameWorks/Streamline.git "<scratchpad>\streamline"
git clone --depth 1 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git "<scratchpad>\ffx"
```
If a repository is gone or renamed, search the official NVIDIA and AMD developer sites and note the URL used.

- [ ] **Step 2: Search the docs**

```
rg -n -i "vulkan" "<scratchpad>\streamline\docs\ProgrammingGuideDLSS_G.md"
rg -n -i "vulkan" "<scratchpad>\streamline\docs\ProgrammingGuide.md"
rg -n -i "VK_NV_low_latency2|VK_KHR_present_wait|vulkan" "<scratchpad>\streamline\docs\ProgrammingGuideReflex.md"
rg -n -i "vulkan" "<scratchpad>\ffx\docs" -g "*frame*"
```
Also read `RELEASE_NOTES.md` in the Streamline checkout for the version where DLSS-G on Vulkan was added or declared unsupported.

- [ ] **Step 3: Write `docs/spikes/S3-fg-vulkan.md`**

Record: Streamline version, whether DLSS-G (and multi frame generation) supports Vulkan, required device extensions and swapchain constraints, Reflex-on-Vulkan requirements, FFX frame generation Vulkan status, and a verdict:
- PASS: DLSS-G runs on Vulkan in the documented SDK. Plan 6 integrates Streamline directly.
- FAIL: DX12-only. Record the fallback: FSR frame generation on Vulkan first, and evaluate a DX12 presentation path as a later option.

---

### Task 7: Spike S4 — compile time of translated-code-sized output

**Files:**
- Create: `tools/spikes/s4_gen.py`, `tools/spikes/s4/CMakeLists.txt`, `docs/spikes/S4-compile-time.md`

**Interfaces:**
- Consumes: N = `unique bl targets` from Task 5.
- Produces: measured clean-build time for N synthetic functions, and the verdict.

- [ ] **Step 1: Write the generator (throwaway)**

`tools/spikes/s4_gen.py`:
```python
# THROWAWAY spike S4: emits N functions shaped like translator output, 200 per .cpp file.
import pathlib
import sys

n = int(sys.argv[1])
out = pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
PER_FILE = 200

HEADER = """#include <cstdint>
#include <intrin.h>
struct Ctx { uint32_t r[32]; double f[32]; uint32_t cr, lr, ctr, xer; uint8_t* mem; };
#define LW(c, a) _byteswap_ulong(*(uint32_t*)((c).mem + ((a) & 0x01FFFFFF)))
#define STW(c, a, v) (*(uint32_t*)((c).mem + ((a) & 0x01FFFFFF)) = _byteswap_ulong(v))
"""

def body(i, n):
    lines = [f"void fn_{i}(Ctx& c) {{",
             "  uint32_t r3 = c.r[3], r4 = c.r[4], r5 = c.r[5], r6 = c.r[6]; double f1 = c.f[1], f2 = c.f[2];"]
    for k in range(18):
        lines += [f"  r5 = LW(c, r3 + {k * 4}); r6 = r5 + r4 * {k + 3}; STW(c, r3 + {k * 4}, r6 ^ {k});",
                  f"  f1 = f1 * f2 + (double)r6; if ((r6 & {k + 1}) == 0) r4 += {k};"]
    lines += [f"  c.r[3] = r3; c.r[4] = r4; c.f[1] = f1;",
              f"  fn_{(i + 1) % n}(c);", "}"]
    return "\n".join(lines)

for start in range(0, n, PER_FILE):
    idx = range(start, min(start + PER_FILE, n))
    decls = "".join(f"void fn_{(i + 1) % n}(Ctx&);\n" for i in idx)
    src = HEADER + decls + "\n".join(body(i, n) for i in idx) + "\n"
    (out / f"gen_{start // PER_FILE:05d}.cpp").write_text(src)
print(f"wrote {n} functions")
```

`tools/spikes/s4/CMakeLists.txt`:
```cmake
# THROWAWAY spike S4. Configure with -DS4_GEN_DIR=<dir produced by s4_gen.py>
cmake_minimum_required(VERSION 3.25)
project(s4 CXX)
file(GLOB SRC CONFIGURE_DEPENDS "${S4_GEN_DIR}/*.cpp")
add_library(s4gen STATIC ${SRC})
target_compile_options(s4gen PRIVATE /O2 /clang:-march=x86-64-v3)
```

- [ ] **Step 2: Generate and build, timed**

Use N from the S1 report (if S1 gave no number, use 25000). In `cmd`:
```
python -I tools\spikes\s4_gen.py <N> "<scratchpad>\s4gen"
call scripts\env.bat
cmake -S tools\spikes\s4 -B "<scratchpad>\s4build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl -DS4_GEN_DIR="<scratchpad>\s4gen"
powershell -NoProfile -Command "Measure-Command { cmake --build '<scratchpad>\s4build' } | Select-Object TotalMinutes"
```
Expected: build completes; record `TotalMinutes`.

- [ ] **Step 3: Write `docs/spikes/S4-compile-time.md`**

Record N, machine (Ryzen 9 7950X, 64 GB), time, and the verdict:
- PASS if ≤ 15 minutes. Translate-then-compile on the user machine is acceptable with the hash-based cache.
- FAIL if > 15 minutes. Record mitigations to test next (unity batching size, `/O1` for cold code, precompiled header for `Ctx`, splitting into several DLLs) and re-measure the best one.
Note the real translator output is larger per function than this synthetic body; state the multiplier risk.

---

### Task 8: Spike gate and next plans

**Files:**
- Modify: `docs/superpowers/specs/2026-10-12-uc-recomp-design.md` (§11: add result lines under each spike)

- [ ] **Step 1: Update the spec**

Under the §11 table add a "Results" list with the verdict and the path of each result document (S1, S3, S4). Add one line: "S2 (motion vectors) moves into the renderer plan because it needs a running game to capture GX state."

- [ ] **Step 2: Report to the user and propose the next plans**

Summarise the three verdicts and the Ghidra decision from Task 5. Propose, in order: Plan 2 analyzer + translator (shape depends on S1), Plan 3 runtime/HLE, Plan 4 renderer (includes S2), then input, launcher, upscalers. Ask which plan to write next.

---

## Self-review (spec coverage)

- §10 Build: Task 4 (`build.bat`, dist, robocopy, non-zero exit). Full pipeline stages (analyzer/translator/compile) arrive in Plan 2; this plan builds and stages only what exists.
- §11 Spikes: S1 Task 5, S3 Task 6, S4 Task 7. S2 deferred to the renderer plan, noted in Task 8 and flagged to the user.
- §2 Toolchain via winget: Task 1. Slang is taken from the Vulkan SDK with a documented fallback.
- Sub-projects 1–7 are intentionally out of this plan.
- Types: `DolSection`, `DolImage`, `DolResult`, `ScanReport`, `SdkString`, `parse_dol`, `section_bytes`, `scan_text`, `find_sdk_strings`, `make_dol`, `put32`, `UC_INSTALL_DIR` are named identically everywhere they appear.
