# Crash reports, GX/audio gap audits and Dolphin FIFO replay — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans (owner chose Native/inline). Steps use `- [ ]`. No automatic commits.

**Goal:** make every failure and every dropped game behaviour visible and attributable, close the gaps the owner reported (crashes, static title background, missing menu click sounds), and add a reference-comparison player that renders Dolphin FIFO logs through our Vulkan renderer.

**Architecture:** (1) a crash reporter writes `debug/crash-*.txt` with host frames mapped to translated guest functions through the dispatch table, the C++ throw-site stack, recent HLE calls and runtime stats. (2) `--gx-audit` attributes guest FIFO commands that no HLE owns to the guest function that wrote them and writes `debug/gx-audit-*.txt`; each important class is then closed by a call-level native HLE with a test. (3) `--audio-audit` reports AX voices by format/state/handling and Wii Remote speaker traffic, then the menu-sound gap is closed natively. (4) `uc_fifo_replay` (test tool, never linked into `uc.exe`) parses Dolphin `.dff` logs, turns the recorded GX command stream into our `gx::FramePacket`s, renders them with `VulkanRenderer`, and writes PNG + diff against a Dolphin frame.

**Tech Stack:** C++20/clang-cl, Win32 (DbgHelp-free: `RtlCaptureStackBackTrace`, `AddVectoredExceptionHandler`), existing uc_runtime/uc_gx/uc_vulkan, Python only for existing route scripts.

**Spec:** `docs/superpowers/specs/2026-10-12-uc-recomp-design.md` (§6 nativity rule, §7 renderer, §11 spikes), `docs/spikes/G20-dolphin-reference.md`.

## Global Constraints

- Runtime stays native: no GX FIFO interpreter, DSP or Wii Remote device model inside `uc.exe`. FIFO parsing exists only in diagnostics (`--gx-audit`) and the offline `uc_fifo_replay` tool.
- Dolphin is an oracle (captures, logs, documentation); no Dolphin code is copied (GPL, emulation architecture).
- Diagnostics are off by default and cost nothing on the normal path except the crash handler.
- Output: `<project>\dist`, install copy to `S:\Games\Umbrella Chronicles`; reports go to `<exe dir>\debug`.
- Files are never deleted without the owner's request; commits only on request. Codex works on `launcher/` in parallel — do not touch it.

## Review Focus

- Crash inside a crash (exception while writing the report, out-of-memory, stack overflow): the report must still be attempted once and the process must exit, never hang.
- Host address outside translated code (runtime, driver, CRT): mapped as "host" without a bogus guest name.
- Concurrent crashes on two threads: one report, no interleaved garbage.
- Audit on a long run: bounded memory (capped attribution samples per key).
- `.dff` with display lists or memory updates outside the recorded ranges, and unknown commands: replay reports and skips them instead of crashing.

---

### Task 1: Crash reports

**Files:** Create `runtime/crash_report.h/.cpp`, `tests/test_crash_report.cpp`; modify `runtime/fatal.cpp`, `runtime/dispatch.cpp/.h`, `runtime/CMakeLists.txt`, `tests/CMakeLists.txt`, `game/uc_main.cpp`.

**Interfaces:**
- `uint32_t guest_function_for_host(const void* pc)` (dispatch.cpp): nearest translated entry at or below `pc` when `pc` lies inside the generated code range, else 0.
- `void crash_report_init(std::filesystem::path directory, std::function<std::string()> stats)`; `void crash_report_write(std::string_view reason, const CONTEXT* context)`; `void crash_note_hle(uint32_t guest_address)` (lock-free ring of the last 64 HLE entries).
- Vectored handler records the stack of the last C++ throw per thread (`0xE06D7363`) for reports from `fatal()`.

- [ ] RED `test_crash_report`: table {0x80001000→fnA, 0x80002000→fnB} maps &fnA's interior to 0x80001000 and an unrelated host address to 0; report writer produces a file containing the reason, "guest 80001000" for a frame inside fnA, and recent HLE entries; two concurrent writers produce one file.
- [ ] Implement; `fatal`, `fatal_guest`, terminate handler and the SEH filter call `crash_report_write` once (atomic guard), then exit.
- [ ] `uc_main` initialises the reporter with `exe_dir()/debug` and a stats lambda (retraces, GX, audio, options). GREEN targeted tests; full suite.

### Task 2: GX gap audit and title background

**Files:** Create `runtime/gx_gap_audit.h/.cpp`, `tests/test_gx_gap_audit.cpp`; modify `game/uc_main.cpp` (FifoSink), `runtime/hle/gx.cpp` (in-primitive query), docs.

**Interfaces:** `void gx_gap_audit_enable(std::filesystem::path report)`; `void gx_gap_audit_write(int size,uint32_t value,bool in_primitive,const void* host_return)`; `std::vector<GapRow> gx_gap_audit_rows()` where `GapRow{std::string key; uint64_t count; std::vector<uint32_t> guest_functions;}`; report written at shutdown.

- [ ] RED: synthetic stream (CP 0x50, XF 0x1018, BP 0xF2, indexed 0x28, CALLDL 0x40, bytes inside a primitive) yields exactly those keys, counts, and no keys from primitive bytes; attribution keeps at most 8 distinct functions per key.
- [ ] Implement; enable through the existing `--gx-audit` option; attribution walks `RtlCaptureStackBackTrace` frames through `guest_function_for_host`.
- [ ] Run title screen with `--gx-audit`, read report; capture Dolphin title frame + FIFO log; identify the dropped class behind the static background (expected: inline CALLDL or texture-matrix/indirect state). Close it with a call-level native HLE + test; rule in the ledger if the evidence points elsewhere.
- [ ] Live check: title background animates (two captures differ in the background region); record in G21.

### Task 3: Audio gap audit and menu sounds

**Files:** Create `runtime/audio_gap_audit.h/.cpp`, `tests/test_audio_gap_audit.cpp`; modify `runtime/ax_audio.cpp`, WPAD HLE file(s), `game/uc_main.cpp`, options.

- [ ] Add `--audio-audit`: counters per AX voice (format, loop, src type, mixer flags, handled/skipped reason) and per WPAD speaker entry (control/stream calls, bytes, format). RED test for the counter keys on synthetic parameter blocks.
- [ ] Run menu route clicking cards/buttons with `--audio-audit`; compare with Dolphin (sound heard there). Identify the missing path (expected: Wii Remote speaker stream or unsupported AX voice feature); implement natively (decode + mix) with fixture test; record debt for anything approximate.
- [ ] Live check: menu clicks produce non-silent PCM in the audio stats; owner confirmation later.

### Task 4: Dolphin FIFO replay tool

**Files:** Create `tools/fifo_replay/{CMakeLists.txt,main.cpp,dff.h,dff.cpp,gx_stream.h,gx_stream.cpp}`, `tests/test_fifo_replay.cpp`; modify top-level `CMakeLists.txt`.

**Interfaces:** `Dff load_dff(path)` (header, CP/XF/BP initial state, frames with FIFO bytes and memory updates); `class GxStream { GxStream(Dff const&, size_t frame); std::vector<gx::FramePacket> packets(); ReplayStats stats; }` converting CP/XF/BP/indexed loads/display lists/primitives into `gx::DrawPacket` (positions/normals/colors/texcoords via `gx::Vertices` with a `gx::Memory` built from memory updates; transforms; channel controls/lights; TEV stages/order/konst/swap; alpha compare, z mode, blend; textures through `decode_texture`; viewport/scissor/projection; fog BP) and EFB copies (`BP 0x52`) into display/texture copies.

- [x] Loader foundation: bounded DFF v5/v6 parsing, register banks, owned file storage, ordered overlapping RAM updates across frames; malformed-input tests and both real captures loaded. No rendering claim yet.
- [x] Geometry conversion tested in `test_fifo_stream`: synthetic colored triangle, projection, position/indexed matrices, display lists, BP masks, truncated/unknown commands and preceding-frame state. Unknown commands fail with frame/list/offset; skipping an unknown-length command would corrupt synchronization.
- [x] Material/texture foundation: TEV, lighting, fog, blend/scissor, RAM texture decode, palette loads, EFB copy bindings; title capture renders to PNG with Vulkan validation.
- [x] Reference PNG loading, absolute RGB mean/peak/changed-pixel metrics, and output-stem-diff.png implemented and verified on deterministic title replay.
- [x] Obtain matching640x480 Dolphin frames through scripts/dolphin_reference.py. First train source verified as zero texture in fresh oracle; explicit diagnostic seeded DFF + SHA256 sidecar. Original live-game prior-frame contents remain DFF-01.
- [x] Loader + stream render packets with VulkanRenderer in a visible non-activating window (owner preference), save requested PNG and output-stem-diff.png with mean/peak error.
- [x] Run train diagnostic seeded capture vs matching Dolphin frame2; record metrics/regions in G20: mean3.662187/255 after offline light-energy unit fix, peak255. Full parity remains open.

### Task 5: Close-out

- [ ] Full `build.bat` (tests, dist, install); docs (G21, tech-debt rows for remaining gaps); ledger; fresh whole-plan review on the strongest model; fix Critical/Important test-first. Then return to the original renderer plan (`2026-10-12-gx-vulkan.md`) checklist audit.
