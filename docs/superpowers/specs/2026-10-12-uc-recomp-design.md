# Umbrella Chronicles Native Port — Design

Date: 2026-10-12. Status: draft for review.

## 1. Goal

Native Windows x64 port of Resident Evil: The Umbrella Chronicles (Wii, RBUE08 NTSC-U) by static recompilation.
Runtime contains no emulator, interpreter, JIT, or PowerPC code. User supplies the extracted game at
`S:\Games\Umbrella Chronicles\game` (`DATA\sys\main.dol`, `DATA\files\...`).

Success criteria (full project):
1. All game code runs as natively compiled x64 (translated ahead of time on the user machine).
2. Native Vulkan renderer, internal resolution up to 4K, aspect ratios incl. 16:9.
3. FPS limits 60 / 120 / 144 / 240 / unlimited.
4. DLSS Super Resolution (primary), FSR (alternative); DLSS/FSR Frame Generation; Reflex.
5. Mouse aim: absolute pointer, no acceleration, no smoothing, minimal latency.
6. Launcher: resolution, DLSS on/off, preset, model, frame generation, game path, FPS limit, Launch button.
7. Later: Ray Tracing, Path Tracing, Ray Reconstruction (design must not block them).

Design priorities (user): maximum nativity, performance, flexibility.

## 2. Decisions already made

| Topic | Decision |
|---|---|
| Renderer | Own native Vulkan 1.3 GX backend. aurora (Dawn/WebGPU) is reference only: WebGPU hides VkDevice/VkImage, which DLSS/FSR/RT need |
| Base | Own project. Wiicompiled, aurora, DolRecomp, Dolphin are references for ideas, not code (avoids GPL-3 inheritance; license is the owner's choice) |
| Launcher | Separate C++ exe, Dear ImGui + SDL3, writes `config.json` |
| Toolchain | Installed via winget after spec approval: VS Build Tools (MSVC + Windows SDK), LLVM/clang-cl, Vulkan SDK, Slang |
| Output | Build to `<project>\dist`, then robocopy to `S:\Games\Umbrella Chronicles` |

## 3. Scope decomposition

Each sub-project gets its own plan and verification.

| # | Sub-project | Output |
|---|---|---|
| 0 | Skeleton + toolchain + `build.bat` | Empty pipeline builds, dist copy works |
| S | Spikes (see §10) | Go/no-go on risks |
| 1 | Analyzer + translator | `main.dol` → C++ sources |
| 2 | Runtime / HLE | Memory, threads, OS, DVD, VI, audio, video, save |
| 3 | Vulkan GX renderer | Picture on screen, res scale, FPS limiter |
| 4 | Input | KPAD/WPAD HLE, mouse pointer |
| 5 | Launcher | ImGui exe + config |
| 6 | Upscalers + frame generation | DLSS (Streamline), FSR (FFX API) |
| 7 | RT / PT / RR | Future |

First milestone (M1): game reaches the Capcom logo, then the title screen.

## 4. Repository layout

```
/build.bat                  one-click build (see §9)
/CMakeLists.txt             top-level, Ninja + clang-cl
/tools/analyzer             DOL parser, function finder, SDK signature matcher
/tools/translator           PPC (Gekko) -> C++ emitter
/runtime                    host runtime linked with generated code
/runtime/os                 OS threads, alarms, message queues, mutexes
/runtime/dvd                file access to extracted tree
/runtime/vi                 retrace, frame pacing
/runtime/ax                 audio (AX -> WASAPI)
/runtime/video              THP / movie decode
/runtime/pad                KPAD/WPAD/PAD HLE, mouse
/renderer                   Vulkan GX backend
/renderer/shaders           Slang sources (TEV, vertex, post)
/upscale                    IUpscaler: DlssStreamline, FsrFfx, None
/launcher                   ImGui launcher
/project                    uc.yml (function map, overrides, patches)
/third_party                vendored deps
/docs/superpowers           specs, plans
/dist                       build output
```

Extension points (flexibility): every subsystem sits behind a small interface (`IRenderer`, `IUpscaler`,
`IAudioSink`, `IInputSource`). Per-game fixes live in `project/uc.yml` and `project/patches/` as data, not hard-coded.

## 5. Translation pipeline (sub-project 1)

1. **Analyzer** parses `main.dol` (sections, BSS, entry). Finds functions by prologue/`blr` scanning, call-graph
   reachability, jump-table detection (`mtctr`/`bctr` patterns), and vtable/function-pointer table scanning in data.
2. **SDK identification**: signature match (Dolphin `totaldb`-style hashes of Nintendo SDK libs: OS, GX, DVD,
   VI, KPAD/WPAD, AX, NAND, MEM, THP, etc.). Matched functions are replaced by HLE implementations and never
   translated. Unmatched Capcom code is translated.
3. **Translator** emits C++ per function:
   - Guest registers (GPR/FPR/CR/XER/LR/CTR) as locals; spilled to a context struct only at calls/blocks that need it.
   - Paired-singles (Gekko PS) via SSE/AVX intrinsics.
   - Memory: single reserved region; access = `base + (addr & mask)` plus byteswap (`movbe`/`bswap`). No per-access translation tables.
   - Direct calls resolved at translation time. Indirect calls (`bctrl`, `blr` to non-LR targets): lookup in a
     dense guest-address → host-function table.
   - Unresolved indirect target = fatal error with guest address logged. No interpreter fallback; missing targets are added to `uc.yml`.
   - Output split into many files for parallel compile.
4. **Compile**: clang-cl, `/O2`, `x86-64-v3`, LTO optional. Result: `uc_game.dll` (or static link into `uc.exe`).
   Cache key = hash(DOL) + translator version.

Known limits: self-modifying and runtime-generated code is unsupported (check the DOL for zero-filled executable
ranges in spike S1; the game has no REL modules).

## 6. Runtime (sub-project 2)

- **Threads**: guest OS threads map to host threads under a global scheduler lock; context switches happen only at
  blocking OS calls (`OSSleepThread`, mutex, message queue, `VIWaitForRetrace`) and respect guest priorities.
  This reproduces the original scheduling without emulating timer interrupts.
- **Time**: `OSGetTime`/alarms backed by a host monotonic clock at the Wii time base (60.75 MHz).
- **DVD**: `DVDOpen/Read/...` served from `DATA\files`. Async reads complete on a worker thread.
- **VI / frame pacing**: `VIWaitForRetrace` is the logic tick. Game logic runs at its native rate; presentation runs
  at the display rate (see §7).
- **Audio**: AX voice mixer on the CPU (SIMD), output via WASAPI. DSP microcode is never emulated: AX is replaced by HLE.
- **Video**: THP/movie decode with a native decoder into a texture.
- **Save**: NAND API mapped to a folder.

## 7. Vulkan GX renderer (sub-project 3)

- HLE at GX call level (`GXBegin`, `GXSetTevOrder`, `GXLoadPosMtxImm`, ...), not FIFO emulation.
- State → pipeline key; pipeline cache persisted to disk; TEV compiled to shaders (Slang → SPIR-V), ubershader
  fallback to avoid hitches on first use.
- Vertex descriptors converted to Vulkan vertex inputs; textures converted/decoded (CMPR, I4, RGB565, ...) on the GPU where possible.
- EFB/XFB: render targets at internal resolution; EFB copies (`GXCopyTex`, `GXCopyDisp`) as image copies/blits.
- Resolution scale to 4K+, aspect ratio handling (16:9 viewport, scissor and HUD anchor fixes via `uc.yml`).
- **Frame structure** (needed by upscalers): pass A scene (HUD-less color + depth + motion vectors), pass B upscale,
  pass C UI at output resolution, pass D present. Orthographic/UI draws are detected by projection type.
- **Motion vectors**: per draw, the previous-frame model/view/projection is tracked by a stable draw identity
  (call-site + object key). Spike S2 validates this before the upscaler work.
- **FPS**: logic rate is fixed by the game. Presentation above logic rate comes from (a) render interpolation of
  tracked object transforms, and (b) DLSS/FSR Frame Generation. Limits: 60/120/144/240/unlocked via a
  frame limiter with a high-resolution wait; VSync/VRR options.
- Ray tracing readiness: acceleration-structure friendly geometry retention (static meshes keep their vertex/index
  buffers), device extensions enumerated but not required.

## 8. Upscaling and frame generation (sub-project 6)

`IUpscaler { init, resize, evaluate(color, depth, mv, jitter, exposure, reset), shutdown }`

- **DlssStreamline**: NVIDIA Streamline (SR, Frame Generation, Reflex). Presets and model selection exposed in the launcher.
- **FsrFfx**: AMD FFX API (FSR upscaling; FSR 4 ML on RX 9000, FSR 3.1.x elsewhere; Frame Generation).
- Latency: Reflex enabled when frame generation is on. Cursor reticle is drawn in pass C with late sampling of the
  mouse position, so aim responsiveness does not depend on the generated frames.
- Future DLSS versions arrive as Streamline/NGX DLL updates behind the same interface.

## 9. Input (sub-project 4) and Launcher (sub-project 5)

**Input.** `KPADRead`/`WPADRead` family replaced by HLE. Source: Windows Raw Input (mouse).
The pointer position is the absolute cursor position mapped to the guest IR coordinate space, with no acceleration
and no filtering. The smoothing present in the statically linked KPAD library is bypassed by replacing the
library functions. Button mapping (Trigger, A/B, reload, etc.) is configurable; gamepad is optional.

**Launcher.** `launcher.exe` (ImGui/SDL3). Fields: game path (default `S:\Games\Umbrella Chronicles\game`),
resolution (incl. 4K), FPS limit (60/120/144/240/unlimited), upscaler (DLSS/FSR/off), DLSS preset, DLSS model,
frame generation mode, Launch button. Stored in `config.json` next to the exe; `uc.exe` reads it and also accepts CLI overrides.

## 10. Build (sub-project 0)

`build.bat` (run manually):
1. Verify tools (MSVC env, clang-cl, CMake, Ninja, Vulkan SDK, Slang); print what is missing.
2. Configure + build `analyzer`, `translator`, `runtime`, `renderer`, `launcher`.
3. If `dist\` cache key differs: run analyzer + translator on `main.dol`, compile generated sources.
4. Stage into `dist\` (`uc.exe`, `launcher.exe`, DLLs, shaders, `project\`).
5. `robocopy dist "S:\Games\Umbrella Chronicles" /E` (copies into the folder root, next to `game\` and `extractor\`).
Exit code non-zero on any failure.

## 11. Spikes (run first, throwaway code)

| ID | Question | Pass criterion |
|---|---|---|
| S1 | How many SDK functions are identifiable by signature in `main.dol`? Any runtime-generated code? | Core OS/GX/DVD/VI/KPAD/AX identified; no zero-filled exec ranges |
| S2 | Can per-draw motion vectors be derived from GX matrix state? | Stable vectors on a captured scene |
| S3 | Does Streamline support DLSS Frame Generation on Vulkan? Which SDK version and extensions? | Documented yes/no plus minimal sample runs on the RTX 4080 |
| S4 | Is Vulkan + Slang + clang-cl build of generated code fast enough? | Full translate+compile within a tolerable time (target under 15 min, cached afterwards) |

If S1 fails (too few identifiable functions), plan falls back to manual identification by call-graph/string
analysis, which extends sub-project 1. If S3 fails, FG goes through FSR-FG or the driver-level path first.

### Results (2026-10-12)

- **S1 — libraries present, identification untested** (`docs/spikes/S1-sdk-scan.md`): ~9.0–9.5k functions; all core SDK libs present by
  banner (RVL_SDK build Apr 2007); no sign of runtime-generated code (the large zero runs are a 0x100-stride vector area, 23 runs; by the
  plan's literal rule this was FAIL and was reclassified after investigation). Function-level identification was not measured.
  Signature DBs for other SDK builds may not match, so the analyzer locates libraries by contiguous layout and anchors, not by a DB alone.
  Ghidra: put to the user.
- **S2 — moved to the renderer plan** (needs a running game to capture GX state).
- **S3 — DLSS documented, not demonstrated; FSR SDK 2.3 FAIL on Vulkan** (`docs/spikes/S3-fg-vulkan.md`): Streamline 2.14.1 documents
  DLSS-G on Vulkan, but Dynamic MFG and VSync+FG are D3D12-only; no sample was run. AMD FSR SDK 2.3 declares Vulkan unsupported;
  FidelityFX SDK 1.x for FSR on Vulkan still has to be checked.
- **S4 — PASS** (`docs/spikes/S4-compile-time.md`): synthetic 9486 functions at 5x the instruction volume compile in ~25 s; real translator
  output (branchy, uneven function sizes) and real link time are still to be measured.

### Results of the analyzer/translator plan (2026-10-12)

- **T1 — PASS** (`docs/spikes/T1-translation-stats.md`): the whole `main.dol` (968,536 instructions, 14,620 functions + 149 secondary entries,
  234 jump tables) translates in 0.6 s and compiles in 19 s; no unresolved switch, no branch outside the known functions.
- **S5 — PARTIAL** (`docs/spikes/S5-identification.md`): 2,771 functions named by Dolphin-format signatures, 30 of 41 required SDK names found
  (OS 119, DVD 78, GX 145, VI 30, WPAD 28 named functions). `OSInit`, `OSPanic`, `OSReport`, `WPADInit/Read`, `KPADInit/Read/SetPosParam` and a few tiny
  functions are still to be located by call graph. Ghidra is not needed yet (`docs/decisions.md`).
- **GX is partly named** (145 `gx.a` functions), so the call-level GX HLE of §7 stays an option next to hardware-level (FIFO) emulation; decide in the runtime/renderer plan.

### Spec corrections from the results

1. §6 Video: this game uses **CRI Sofdec** (`.sfd`, 59 files), not THP/Bink. Sofdec runs as translated code; the runtime provides texture upload of decoded frames.
2. §6 Audio: add CRI ADX (translated code on top of AX) and DSP-ADPCM decoding in the HLE AX mixer (1334 `.brstm` streams).
3. §7/§8 Frame generation: fixed multiplier only (no Dynamic MFG; offer what `numFramesToGenerateMax` reports, likely 2x on the RTX 4080),
   VSync off while FG is active, present loop follows the Streamline Vulkan binary-semaphore handshake, frame limiter is our own.
   Plan 6 starts with a minimal DLSS-G sample on Vulkan.
4. §8 `FsrFfx`: FidelityFX SDK 1.x for Vulkan (FSR 3.1 upscaler); FSR frame generation on Vulkan is unconfirmed, driver-level FG is the interim option.

## 12. Risks

1. SDK identification without symbols (largest risk, S1).
2. Motion vectors for a GX-style draw stream (S2).
3. DLSS-FG on Vulkan availability (S3).
4. Wii-specific effects in the renderer (EFB peeks/pokes, copy filters, indirect textures) found during bring-up.
5. Game logic timing coupled to the 60 Hz retrace; interpolation must not alter logic.
6. Legal: no game data or translated code is distributed; user provides the dump. License to be decided by the owner.

## 13. Out of scope for now

RT/PT/RR implementation (only readiness), online features, mods, other regions, Linux/macOS.
