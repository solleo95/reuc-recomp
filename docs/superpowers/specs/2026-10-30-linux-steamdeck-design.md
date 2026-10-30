# Native Linux and Steam Deck port: design

## Intent

The owner wants the port to run natively on Linux and play well on a Steam Deck, with gyro aiming. A native build avoids Proton's overhead and lets the port use the Deck's sensors and handheld features directly. The owner owns a Steam Deck, which is reachable from the PC as `ssh deck`, so builds can be run and measured remotely.

## Goals

1. `uc` builds for x86-64 Linux from the Windows PC (cross build, no WSL) and runs on SteamOS: windowed Vulkan rendering, audio, keyboard/mouse and gamepad input, saves, the in-game menu.
2. The existing test harness (`uctest`) can drive `uc` on the Deck over `ssh deck`, so FPS runs, demos and screenshots need no one at the Deck.
3. Phase 2 (a separate plan): Deck adaptation: gyro aiming, 1280x800 layout, Deck button glyphs, power and frame-rate profiles, packaging.

## Non-goals (first version)

Launcher (`uc_launcher`), updater, netplay, Wii Remote HID, native-DLL mods (Lua/JS mods are in scope if they build), DLSS / Streamline / NGX / DLSS 5, Direct3D 11/12 backends. Windows behavior must not change.

## Anti-goals

Any change that slows or alters the Windows build, its tests or its saves. Leftover files on the Deck (the owner has ~47 GB free): every run cleans up after itself.

## Constraints

- Cross build: LLVM from `C:\Program Files\LLVM` (`clang++`, `ld.lld`, `llvm-ar`) with `--target=x86_64-linux-gnu` against the Steam Runtime sniper SDK sysroot (`D:\Toolchains\sniper-sysroot`, glibc 2.31, gcc 10 libstdc++). Windows cannot create symlinks without rights; the sysroot uses copies and junctions.
- The translator and `slangc` run on Windows; `generated/` and the SPIR-V shaders are platform-independent inputs of the Linux build.
- C++20, `-march=x86-64-v3` (the Deck's Zen 2 supports it).
- Branch `feature/linux-steamdeck`. The game image stays the user's own (`RBUE08`); the Deck needs the extracted `game` folder.
- Vulkan 1.3 on RADV; SDL3 (fetched, built for Linux by the cross build) for window, audio, input.

## How it gets made

Pairs of platform files chosen in CMake (`*_win.cpp` / `*_posix.cpp`) where the code differs wholesale (guest memory, save file system, crash handling, test pipe); `#ifdef _WIN32` for small differences (clock, thread affinity, entry point). Wide-char paths go through two helpers so shared code stays free of `_wfopen` and `wstring`. CMake targets that only exist for Windows (launcher, updater, D3D, Streamline, Wii Remote, `.rc`) are guarded by `WIN32`. This is the builder's call within those rules.

## Left to the builder

Names of helper functions inside the stated files, the exact crash-report format on Linux, and which of the 167 CTest tests build on Linux (all that do not need Windows APIs; the plan lists the first set).
