# Linux Port Bring-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `uc` cross-builds from the Windows PC for Linux and runs windowed on the Steam Deck (reached as `ssh deck`) with Vulkan, audio, input and saves; Windows stays unchanged.

**Architecture:** One CMake tree. The Linux configure uses a toolchain file (clang + lld + the sniper sysroot); Windows-only targets are guarded by `WIN32`. Code that differs wholesale is split into `*_win.cpp` / `*_posix.cpp` chosen in CMake; small differences use `#ifdef _WIN32`. A helper script runs binaries on the Deck and cleans up after itself.

**Tech Stack:** C++20, CMake/Ninja, LLVM cross toolchain, Steam Runtime sniper sysroot, SDL3, Vulkan 1.3 (RADV), slangc, Python 3 for helpers, OpenSSH.

**Spec:** `docs/superpowers/specs/2026-10-30-linux-steamdeck-design.md`

## Global Constraints

- Windows build, tests and saves stay behaviorally identical (`build.bat --no-install` must still pass after every task).
- Sysroot: `D:\Toolchains\sniper-sysroot`; compiler `C:\Program Files\LLVM\bin\clang++.exe`; flags `--target=x86_64-linux-gnu -fuse-ld=lld -std=c++20 -march=x86-64-v3`.
- Linux build dir: `build-linux` (git-ignored by `/build-*/`).
- Everything copied to the Deck lives in `~/uc-test` and is deleted after the run; the owner has ~47 GB free on `/home` and the Steam library and saves there are never touched.
- Edit source only with Edit/Write (no heredoc or sed patches; backslashes break).
- Out of scope: launcher, updater, netplay, Wii Remote HID, native-DLL mods, DLSS/Streamline/NGX/DLSS 5, D3D11/12.

## Review Focus

- Paths with spaces and non-ASCII characters (`~/uc-test/Umbrella Chronicles/сохр`): save, log and config paths work (Task 6, Task 7).
- Power loss or crash during a save: the old save file survives intact (Task 6).
- `mmap` of the 4 GiB guest range fails (low `vm.max_map_count`, address-space limits): the game exits with a readable message, not a hang (Task 4).
- A SIGSEGV inside a Vulkan driver thread: the crash handler writes a report and exits non-zero, not a loop (Task 5).
- A 1 ms sleep takes about 1 ms (no 4 ms granularity), so frame pacing at 120 Hz+ holds (Task 5).

---

### Task 1: Linux toolchain, configure and Deck helper

**Files:**
- Create: `cmake/toolchains/linux-sniper.cmake`, `scripts/linux/extract_sysroot.py`, `scripts/linux/build.ps1`, `scripts/linux/deck.py`
- Modify: `CMakeLists.txt`, `common/CMakeLists.txt`, `runtime/CMakeLists.txt`
- Test: `scripts/linux/deck.py selftest` (runs a cross-built hello on the Deck)

**Interfaces:**
- Produces: `scripts/linux/build.ps1 [-Target <t>]` configures/builds `build-linux`; `python scripts/linux/deck.py push <file...>`, `deck.py run <exe> [args...]`, `deck.py clean`, `deck.py selftest`. `run` and `selftest` always end by deleting `~/uc-test` and printing `df -h /home`. CMake variable `UC_LINUX` is ON when `CMAKE_SYSTEM_NAME STREQUAL Linux`.

- [ ] **Step 1:** Copy the extraction script that produced the sysroot (it keeps headers and x86_64 libs, copies file symlinks, makes junctions from `links.txt`) to `scripts/linux/extract_sysroot.py` with a usage docstring; add the junction step to it so one command rebuilds the sysroot.
- [ ] **Step 2:** `cmake/toolchains/linux-sniper.cmake`: set `CMAKE_SYSTEM_NAME Linux`, `CMAKE_SYSTEM_PROCESSOR x86_64`, compilers `clang`/`clang++` from `C:/Program Files/LLVM/bin`, `CMAKE_SYSROOT` and `--target`, `-fuse-ld=lld`, `CMAKE_AR llvm-ar`, `CMAKE_RANLIB llvm-ranlib`, `CMAKE_FIND_ROOT_PATH_MODE_*` ONLY for libraries/includes, `CMAKE_CXX_FLAGS_INIT -march=x86-64-v3 -pthread`.
- [ ] **Step 3:** Root `CMakeLists.txt`: `project(uc LANGUAGES C CXX)` plus `enable_language(RC)` only `if(WIN32)`; wrap the `/clang:` flags, PGO and ThinLTO blocks in `if(WIN32)`; add `if(WIN32) ... endif()` around `updater`, `launcher`, `mods-examples/hello_native` and the `InstallRequiredSystemLibraries` block.
- [ ] **Step 4:** `common/CMakeLists.txt`: link `ws2_32` only on WIN32; guard `uc_windows_args`, `uc_client_update_lock` with `WIN32`.
- [ ] **Step 5:** `scripts/linux/deck.py`: `push` tars the files over `ssh deck "tar x -C ~/uc-test"`; `run` pushes, executes, streams output, returns the exit code, then runs `clean`; `clean` removes `~/uc-test` and prints free space; `selftest` builds nothing, it pushes a prebuilt `hello` and expects `hello linux`.
- [ ] **Step 6:** Verify: `pwsh scripts/linux/build.ps1 -Target uc_common` → configure and build succeed (SDL, mimalloc, nlohmann_json fetch-and-build is deferred to Task 8; limit the configure to targets that exist so far with `-DUC_LINUX_BRINGUP=ON` skipping `renderer`, `game`, `tests`). Run `python scripts/linux/deck.py selftest` → prints `hello linux 4 1` and the free-space line.
- [ ] **Step 7:** Windows regression: `build.bat --no-install` passes. Commit `Linux: cross toolchain file, WIN32 guards and Deck run helper`.

### Task 2: Byte-swap shim for non-MSVC targets

**Files:**
- Create: `runtime/compat.h`, `tests/test_compat_bswap.cpp`
- Modify: `runtime/ppc_rt.h` (include it), `tests/CMakeLists.txt`

**Interfaces:**
- Produces: when `_WIN32` is not defined, `runtime/compat.h` defines `_byteswap_ushort`, `_byteswap_ulong`, `_byteswap_uint64` as the `__builtin_bswap16/32/64` equivalents; nothing on Windows.

- [ ] **Step 1: Write the failing test** `tests/test_compat_bswap.cpp`: `_byteswap_ulong(0x01020304u) == 0x04030201u`, `_byteswap_ushort(0x0102) == 0x0201`, `_byteswap_uint64(0x0102030405060708ull) == 0x0807060504030201ull`; return non-zero on mismatch. Add `add_executable`/`add_test(test_compat_bswap)`.
- [ ] **Step 2:** Run on Windows (`ctest -R test_compat_bswap`): FAIL to build (header missing).
- [ ] **Step 3:** Create `runtime/compat.h` per Interfaces and include it from `ppc_rt.h` before the first use.
- [ ] **Step 4:** Windows `ctest -R test_compat_bswap` passes; cross-build `test_compat_bswap` and run it via `deck.py run` → exit 0.
- [ ] **Step 5:** Cross-build `uc_runtime` objects that include `ppc_rt.h` (compile only, with `-DUC_LINUX_BRINGUP`): no `_byteswap` errors. Commit.

### Task 3: POSIX clock, timers and thread affinity

**Files:**
- Modify: `runtime/clock.cpp`, `runtime/sched.cpp` (thread creation, wait, affinity), `runtime/CMakeLists.txt`
- Test: `tests/test_clock_sleep.cpp`

**Interfaces:**
- Consumes: `uc::now_ticks()` from `clock.h` (unchanged API).
- Produces: `uc::now_ticks()` from `clock_gettime(CLOCK_MONOTONIC)` scaled to the existing tick rate; scheduler threads via `pthread_create` with a `host_stack_`-sized stack and `pthread_setaffinity_np` for `pin_cpu_`.

- [ ] **Step 1: Write the failing test** `test_clock_sleep`: `now_ticks()` is monotonic over 1000 reads; 200 sleeps of 1 ms through the project's high-resolution sleep helper (see Task 8's `uc::sleep_precise` in `game/`; for this test call `clock_nanosleep` directly) average under 1.6 ms.
- [ ] **Step 2:** Run on Deck (cross-built): FAIL until `clock.cpp` has the POSIX branch.
- [ ] **Step 3:** Implement the `#ifdef _WIN32` split in `clock.cpp` and `sched.cpp`'s `tramp`/`CreateThread`/`SetThreadAffinityMask`/`WaitForSingleObject(handle,5000)` joins (use `pthread_timedjoin_np`).
- [ ] **Step 4:** `deck.py run test_clock_sleep` → PASS. Windows `ctest` for existing scheduler tests (`-R sched`) still passes. Commit.

### Task 4: Guest memory with memfd

**Files:**
- Create: `runtime/guest_mem_win.cpp` (moved from `guest_mem.cpp`), `runtime/guest_mem_posix.cpp`
- Modify: `runtime/CMakeLists.txt`
- Test: `tests/test_guest_mem_alias.cpp`

**Interfaces:**
- Produces: same API as `guest_mem.h` (`guest_mem_init()`, `guest_mem_base()`); on failure prints `fatal: cannot map guest memory (<strerror>)` and `std::exit(4)`.

- [ ] **Step 1: Write the failing test:** after `guest_mem_init()`, a byte written at `base+0x80000010` reads back at `+0x00000010`, `+0xC0000010`; MEM2 likewise through `0x90000000`/`0xD0000000`/`0x10000000`; locked cache `0xE0000000` aliases `0x20000000`; reading `0xCC000000` raises SIGSEGV (checked via a forked child).
- [ ] **Step 2:** Implement the POSIX file: one `memfd_create` per section (`ftruncate` to size), reserve 4 GiB with `mmap(PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)`, then `mmap(MAP_SHARED|MAP_FIXED)` each view at `base+at[i]`, same table `kViews` as the Windows file; retry loop kept (16 attempts).
- [ ] **Step 3:** Cross-build and `deck.py run test_guest_mem_alias` → PASS. Also run with `ulimit -v 2000000` in the same call and expect exit code 4 with the readable message (Review Focus: mmap failure).
- [ ] **Step 4:** Windows `ctest` unchanged. Commit.

### Task 5: Crash handling, fatal path and write-trap off

**Files:**
- Create: `runtime/fatal_posix.cpp`, `runtime/crash_report_posix.cpp`
- Modify: `runtime/fatal.cpp`, `runtime/crash_report.cpp` (Windows halves into `#ifdef _WIN32` or `_win.cpp` files), `game/write_trap.cpp` (non-Windows stub), `game/CMakeLists.txt`, `runtime/CMakeLists.txt`
- Test: `tests/test_crash_report_posix.cpp`

**Interfaces:**
- Consumes: `crash_report.h` / `fatal.h` public functions as they exist (`crash_guard_thread()`, `fatal_guest(addr, what)`).
- Produces: `SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT` handlers on an alternate stack that write the same report file as Windows (signal, fault address, guest thread id, registers via `ucontext`) then `_exit(139)`.

- [ ] **Step 1: Write the failing test:** a child process installs the handler, dereferences null in a guest-marked thread; the parent expects a report file in a temp dir and exit status 139; a second child faults inside a non-guest thread with the same result (no loop).
- [ ] **Step 2:** Implement per Interfaces. The handler resets the signal to `SIG_DFL` first so a second fault cannot loop.
- [ ] **Step 3:** `write_trap.cpp`: on non-Windows, `install_write_trap()` and friends compile to no-ops returning false (a debugging tool; documented in the file's header comment).
- [ ] **Step 4:** `deck.py run test_crash_report_posix` → PASS. Windows `ctest -R crash` passes. Commit.

### Task 6: POSIX save file system

**Files:**
- Create: `runtime/savefs_posix.cpp` (the Windows implementation moves to `savefs_win.cpp`)
- Modify: `runtime/savefs.h` (types behind `#ifdef`), `runtime/CMakeLists.txt`
- Test: existing `test_savefs*` cases built for Linux plus `tests/test_savefs_atomic_posix.cpp`

**Interfaces:**
- Consumes: the `SaveFs` public methods declared in `savefs.h` (open, read, write, seek, close, rename, remove, list, directory ops with the same negative error codes as `error(DWORD)` maps: `-108` disk full, `-101` invalid, `-102` internal name).
- Produces: a POSIX implementation with identical return codes, using `open/openat`, `fsync`, `renameat2(RENAME_NOREPLACE)` where the Windows code uses no-replace semantics, `errno` → the same codes.

- [ ] **Step 1: Write the failing atomic test:** write a file through the SaveFs API with `fsync` on close; simulate a crash between temp write and rename by killing the child; the previous file content is intact. Also open/read/write/list a directory named `сохр aaa/слот 1` (spaces and non-ASCII).
- [ ] **Step 2:** Implement the file by mirroring the Windows file's behavior section by section; internal names (`internal(name)`) rejected the same way.
- [ ] **Step 3:** Build the existing savefs tests for Linux; `deck.py run` each → PASS; Windows `ctest -R savefs` unchanged. Commit.

### Task 7: Common helpers: paths, logs, CPU topology, config

**Files:**
- Create: `common/path_io.h`, `common/path_io.cpp`
- Modify: `common/diagnostic_log.cpp`, `common/cpu_topology.cpp`, `common/input_config.cpp`, `common/file_rename.h`, `common/public_release.h`, `common/CMakeLists.txt`
- Test: `tests/test_path_io.cpp`

**Interfaces:**
- Produces: `std::FILE* uc::fopen_path(const std::filesystem::path&, const char* mode)` (Windows: `_wfopen` with the mode widened; POSIX: `fopen`), `std::filesystem::path uc::exe_path()` (Windows `GetModuleFileNameW`, Linux `readlink("/proc/self/exe")`), `std::filesystem::path uc::user_data_dir()` (Linux: `$XDG_DATA_HOME/umbrella-chronicles` or `~/.local/share/...`; Windows: the exe directory as today), `bool uc::open_folder(const std::filesystem::path&)` (Windows `ShellExecuteW`, Linux `xdg-open`).

- [ ] **Step 1: Write the failing test:** round-trip a file at a path containing a space and `Привет` through `fopen_path`; `exe_path()` ends with the test's own name; `user_data_dir()` honors `XDG_DATA_HOME` when set.
- [ ] **Step 2:** Implement; then replace direct `_wfopen`, `GetModuleFileNameW`, `ShellExecuteW` and the Win32 calls in `diagnostic_log.cpp`, `cpu_topology.cpp` (use `sched_getaffinity` and `/sys/devices/system/cpu` for core grouping), `input_config.cpp`, `file_rename.h` (`std::filesystem::rename` / `renameat2` on Linux).
- [ ] **Step 3:** `deck.py run test_path_io` → PASS; Windows `ctest -R "path_io|diagnostic|input_config|cpu_topology"` passes. Commit.

### Task 8: Game executable pieces: entry point, timers, audio, test server transport

**Files:**
- Modify: `game/uc_main.cpp` (entry `main` on Linux; waitable timer → `clock_nanosleep`; `timeBeginPeriod`/power throttling/affinity/DWM and the diagnostic-window raise under `#ifdef _WIN32`; `SetEnvironmentVariableA` → `setenv`), `game/audio_output.cpp`, `game/compile_screen.cpp`, `game/frame_log.cpp`, `game/system_report.cpp`, `game/cpu_sampler.cpp`, `game/det_trace.cpp`, `game/options.cpp`, `game/test_pipe.h/.cpp`, `game/CMakeLists.txt`
- Create: `game/test_pipe_posix.cpp`, `game/sleep_precise.h`
- Test: `tests/test_pipe_posix.cpp`

**Interfaces:**
- Produces: `void uc::sleep_precise(std::chrono::nanoseconds)` (uses `clock_nanosleep` on Linux); `TestPipes` keeps its class interface; the POSIX transport is a Unix-domain socket pair at `$XDG_RUNTIME_DIR/uc-test-<name>.sock` for commands and events (single client each), so `uctest` can reach it through `ssh -L`.

- [ ] **Step 1: Write the failing test** `test_pipe_posix`: a client connects to both sockets, sends `ping\n`, `take_lines()` returns `ping`; `send("evt", true)` arrives at the client; `disconnected()` becomes true after the client closes.
- [ ] **Step 2:** Implement `test_pipe_posix.cpp` and guard the Windows file; change `uc_main.cpp` entry: `int main(int argc, char** argv)` on Linux builds the same argument vector that `wmain` builds (UTF-8 → `std::wstring` is not needed if the code takes `std::string` arguments; where it takes `wstring`, convert once in a `#ifdef` block).
- [ ] **Step 3:** `audio_output.cpp`: SDL audio only, drop any WASAPI-specific code under `#ifdef _WIN32`; `compile_screen.cpp`/`system_report.cpp`: Linux sources for GPU/driver/CPU/RAM text (`/proc/cpuinfo`, `/proc/meminfo`, Vulkan device properties).
- [ ] **Step 4:** `deck.py run test_pipe_posix` → PASS. Windows `ctest -R "test_pipe|test_server|frame_log|system_report"` passes. Commit.

### Task 9: Renderer on Linux (Vulkan only)

**Files:**
- Modify: `renderer/CMakeLists.txt` (guard `uc_directx`, Streamline, NGX, DLSS5 SDK targets, `windowscodecs`, `patch_sdl.cmake` with `WIN32`; add Linux `Vulkan::Vulkan` from the Windows Vulkan SDK headers plus the sysroot's `libvulkan.so`), `renderer/vulkan_renderer.cpp` (`windows.h` and the HWND/Streamline surface path under `#ifdef _WIN32`), `renderer/png.cpp` (PNG encode through a header-only encoder in `third_party/` or stb; `uc::write_png` signature unchanged), `renderer/prompt_picture.cpp`, `renderer/upscaler_probe.cpp`, `renderer/native_*.cpp` and `renderer/fsr_*.cpp` (compile with FSR/DLSS/Streamline behind `UC_HAVE_NVIDIA` / `UC_HAVE_FFX`; both off in the first Linux build)
- Test: `tests/renderer_smoke` (existing) built for Linux

**Interfaces:**
- Consumes: `UpscalerKind` and `streamline_` as they exist; on Linux `streamline_` is always null and the FSR/DLSS options report "unavailable" through the existing probe.
- Produces: `uc_vulkan` library for Linux with the native upscalers (SMAA, RCAS, native frame generation) only.

- [ ] **Step 1:** Cross-build shaders on Windows with `slangc` (same custom commands) into `build-linux/renderer/shaders`; no change in the shader sources.
- [ ] **Step 2:** Guard the Windows-only code as listed; add a `png` writer test: `write_png` of a 2x2 BGRA image decodes back to the same pixels (decoder in the test or `file` signature + size check).
- [ ] **Step 3:** Build `renderer_smoke` for Linux; `deck.py run` it in Game Mode with `DISPLAY=:1` (gamescope's XWayland): it renders its frame and exits 0. Capture one frame to PNG and pull it back with `scp` (then delete it on the Deck).
- [ ] **Step 4:** Windows `build.bat --no-install` passes. Commit.

### Task 10: Link `uc` and first launch on the Deck

**Files:**
- Modify: `game/CMakeLists.txt` (`uc` target: Linux library list without `dxgi`, `winmm`, `shell32`, `uc_directx`, `uc_wiimote_input`, `uc_client_update_lock`, `assets/app.rc`, `/MAP`; Linux link line gets SDL3, `vulkan`, `pthread`, `dl`; `mimalloc` builds as on Windows), `modding/CMakeLists.txt` (no native-DLL runtime on Linux; Lua and JS runtimes stay), `launcher/CMakeLists.txt` pieces that `uc` links (`uc_ui_style`, `uc_binding_editor`, `uc_gamepad_panel`, `uc_launch`, `uc_launcher_locale`) become WIN32-independent or are replaced by in-game equivalents
- Create: `game/wiimote_stub.cpp` (non-Windows `uc_wiimote_input` with the same public header, reporting "no remote")
- Test: `deck.py smoke` (Step 3)

**Interfaces:**
- Produces: `build-linux/game/uc`; `scripts/linux/deck.py smoke` pushes `uc`, `libSDL3.so*`, `data/shaders`, `data/pipeline_cache` seed, and runs `./uc --smoke` style command (the existing `uc_smoke` target) against the extracted `game` folder at `~/uc-game` (pushed once, 3.8 GB, removed by `deck.py clean --all`).

- [ ] **Step 1:** Fix every remaining compile/link error in the `uc` target by guarding or porting per the earlier tasks' patterns; keep each fix in the file that owns it.
- [ ] **Step 2:** `uc_smoke` (guest boot without a window) cross-builds; `deck.py run uc_smoke` reaches the same final log line as on Windows.
- [ ] **Step 3:** `deck.py smoke`: launches `uc --skip-intro --mute --width 1280 --height 720 --seconds 20` windowed under `DISPLAY=:1`, exits 0, `logs/*/frames.csv` shows at least 100 presented frames; pull `frames.csv` and one screenshot (`--capture-dir`) to `build-linux/deck-runs/` and delete the Deck copies.
- [ ] **Step 4:** Windows `build.bat` (full, with tests) passes. Commit `Linux: uc builds and starts on the Steam Deck`.

### Task 11: `uctest` over ssh

**Files:**
- Modify: `scripts/uctest/client.py` (transport selection), `scripts/uctest/PipeTransport` module (new `SshSocketTransport`), `scripts/uctest/__main__.py` (flag `--deck`)
- Test: `scripts/uctest/selftest.py` case `deck_ping`

**Interfaces:**
- Produces: `--deck` makes `uctest` push the build, start `uc` with `--test-server <name>` on the Deck, forward the two sockets over `ssh -L`, run the scenario, fetch artifacts, and clean the Deck. Windows runs are unchanged.

- [ ] **Step 1: Write the failing selftest case** `deck_ping`: with `--deck`, `ping` returns `pong` from the Deck's game within 10 s and the Deck's `~/uc-test` no longer exists afterwards.
- [ ] **Step 2:** Implement the transport; reuse the line protocol unchanged.
- [ ] **Step 3:** `python -m uctest run train_gameplay --deck --speed 8` reaches the train and exits 0; `python -m uctest bench` stays Windows-only for now (noted in `docs/testing/test-server.md`).
- [ ] **Step 4:** Update `docs/testing/test-server.md` and `AGENTS.md` with the Linux/Deck commands. Commit.

## Self-review notes

- Spec coverage: goals 1 → Tasks 1-10; goal 2 → Task 11; goal 3 and packaging are a separate plan (gyro, Deck layout, profiles, Flatpak/AppImage), started once the Task 10 smoke passes and an FPS baseline is read from `frames.csv`.
- Names used across tasks: `uc::fopen_path`, `exe_path`, `user_data_dir`, `open_folder` (Task 7) are consumed in Task 8; `sleep_precise` (Task 8) is introduced after Task 3's test, which uses `clock_nanosleep` directly for that reason.
