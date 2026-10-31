# Performance Roadmap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Raise FPS on the Steam Deck and Windows at the owner's settings (per-pixel lighting, ultra motion blur, bloom 150) by cutting GPU critical-path cost, then find and (only with proof) parallelise independent guest loops.

**Architecture:** Part A changes shared Slang shaders and the three backends' motion-blur/bloom passes, each change guarded by an exactness test and a Deck/Windows measurement. Part B adds call-stack profiling so the guest thread's inclusive cost per function is known. Part C is gated on Part B: a memory-access tracer proves loop independence, then a worker pool runs one proven loop with a serial reference path.

**Tech Stack:** C++20, Slang shaders (`renderer/shaders/*.slang`, compiled for Vulkan/D3D11/D3D12), Vulkan 1.3, D3D12/D3D11, Python 3.13 harness (`scripts/uctest`), cross-built Linux binary for the Deck (`scripts/linux/build.ps1`).

**Spec:** `docs/superpowers/specs/2026-10-30-performance-roadmap-design.md`

## Global Constraints

- Every change helps Windows and the Steam Deck, or is marked as one of them. Shaders are shared; passes exist once per backend (Vulkan `motion_blur_pass.cpp`/`bloom_pass.cpp`, D3D12 `d3d12_renderer.cpp`, D3D11 `d3d11_renderer.cpp`): change all three or say why not.
- No change of GPU clocks, default render scale or default effect settings. No picture change at default quality unless a before/after image diff is shown to the owner first.
- No automatic multithreading of guest threads; no thread offload of GX.
- Guest-code changes stay exact: differential test against the previous path and `uctest demo check`.
- Measure with the owner's settings (`--per-pixel-lighting --motion-blur ultra --bloom-strength 150 --fps-limit 90`) and the defaults; alternate builds, at least 2 runs each; report Max FPS, mean, lowest second, p99/worst frame, and `[gpu-sum]`. Keep a change when it is correct and not slower at both settings.
- Before any Deck run: `ssh deck pgrep -a uc` must print nothing (the owner may be playing). After: delete test binaries, `autotest`, `next-run.sh` from `~/uc-run`. Upload binaries with `ssh deck 'cat > f' < file`, never a `cat | ssh` pipe.
- Use `py -3.13` for `uctest demo`/`bench` (3.10 fails on an f-string in `demo.py`); run the game exe from the game folder, not `dist/uc.exe` (no game data). No test, bench or smoke run fullscreen or desktop-sized.
- Edit source only with Edit/Write (never heredoc/sed/python patches). After an edit containing an escape sequence, grep the changed line before the first build.
- No commits unless the owner asks: each task ends with a proposed commit message in the report, not a `git commit`. At the end of a finished task run the full `build.bat` and deploy to the game folder (per `~/.claude/rules/build-and-deploy.md`); the final branch review runs on Opus only (`model: "opus"`).

## Review Focus

- A lit draw whose colour and alpha channel controls differ (alpha-only lighting) must render pixel-identically after the light-loop merge.
- Per-pixel lighting with channel 0 lit and channel 1 unlit (and the reverse): skipping vertex lighting must be per channel.
- Motion blur with zero or sub-pixel vectors, a camera cut, and render size ≠ output size: no flicker, no NaN, still-scene output equals the unblurred frame.
- Runtime toggles on D3D12/D3D11: switching motion blur off/on and a resolution change recreate the effect images without a stale or half-sized copy.
- A parallel loop on a 1-core machine (worker count 0) and under netplay: serial path taken, state hashes equal.
- The profiler/tracer switched off (env unset, flag off) has no measurable cost and does not change output.

## File Structure

| File | Responsibility |
|---|---|
| `scripts/uctest/runsummary.py` (new) | Parse a run log into FPS/pacing/GPU-category summaries |
| `scripts/uctest/test_runsummary.py` (new) | Unit tests for it |
| `scripts/linux/deck_perf.py` | `--owner` preset, prints summaries |
| `renderer/shaders/smoke.slang` | Light loop once per channel, no duplicate vertex lighting, cheaper loop |
| `renderer/shaders/motion_blur.slang`, `renderer/motion_blur_pass.cpp` | Adaptive sample count, region copy, precise barriers |
| `renderer/d3d12_renderer.cpp`, `renderer/d3d11_renderer.cpp` | Same motion-blur plumbing fixes |
| `tests/renderer_effects_smoke.cpp` | New sections `ppl-exact`, `blur-adaptive` |
| `game/cpu_sampler.cpp` | Linux stack sampling (`UC_CPU_SAMPLE_STACKS`) |
| `scripts/inclusive_profile.py` (new) + `scripts/test_inclusive_profile.py` | Inclusive time per function from stack samples |
| `docs/spikes/P1-effects-results.md`, `P2-guest-inclusive-profile.md`, `P3-loop-independence.md` (new) | Results the next tasks read |
| `runtime/ppc_rt.h`, `runtime/mem_trace.{h,cpp}` (new) | `UC_TRACE_MEM` access tracer |
| `runtime/par_for.{h,cpp}` (new) + `tests/test_par_for.cpp` | Worker pool, serial fallback |

---

## Part A: effects on the GPU

### Task 1: Run summaries in the repo

**Files:**
- Create: `scripts/uctest/runsummary.py`, `scripts/uctest/test_runsummary.py`
- Modify: `scripts/linux/deck_perf.py`

**Interfaces:**
- Produces: `fps_summary(log: str, seconds: int) -> dict` with keys `fps_mean, fps_min, fps_max, frame_p50, frame_p99_mean, frame_p99_worst`; `gpu_summary(log: str, seconds: int) -> dict` with keys `scene, efb, copies, bloom, upscale, blur_ms_frame, render_ms_s, gpu_total_ms_s`; `python -m uctest.runsummary [run_dir]` prints both for the newest run.

- [ ] **Step 1: Write the failing test** `RunSummary.test_fps_and_gpu_lines`: a literal 3-second log (`[fps] window_ms 1000.1 retraces 1:61 presents 70 fps 69.9`, `[frametime] n 70 avg 14.3 p50 14.2 p99 16.0 max 16.0 ms`, `[gpu-sum] per second: scene draws 300.0 ms, efb draws 90.0 ms, texture copies 80.0 ms, bloom 40.0 ms, upscale 6.0 ms`, `[gpu] passes: ... motion blur 1.20 ms ... bloom 0.50 ms`, `[perf] presents 70 render 400.00 ms ...`, three seconds of varying values) asserting every key against hand-computed means, and `fps_max` equals the largest `fps`.
- [ ] **Step 2:** `cd scripts && py -3.13 -m unittest uctest.test_runsummary -v` → FAIL (module missing).
- [ ] **Step 3:** Implement both functions in `runsummary.py` with the regexes from the session's `pacing.py`/`gpusum.py` (the `[fps]`, `[frametime]`, `[gpu-sum]`, `[gpu] passes`, `[perf]` line shapes above); `main` finds the newest `build/autotest/*/run.log`.
- [ ] **Step 4:** Add `--owner` to `deck_perf.py`: appends `--game-arg` pairs for the owner's four arguments (list in Global Constraints) and, after each run, prints `fps_summary` and `gpu_summary` for the newest run.
- [ ] **Step 5:** Run the unit test → PASS; `py -3.13 scripts/linux/deck_perf.py --owner uc` once (Deck free) prints FPS plus both summaries. Add a ctest wrapper in `tests/CMakeLists.txt` in the form of `test_uctest_steps`.

### Task 2: Pixel-exactness characterisation for lighting

**Files:**
- Modify: `tests/renderer_effects_smoke.cpp` (new section `ppl-exact` after `ppl`, ~line 517)

**Interfaces:**
- Produces: section `ppl-exact` (run with `renderer_effects_smoke --ppl-exact`) rendering six lit quads and comparing an FNV-1a 64-bit hash of each capture to `kPplGolden[6]`; `UC_GOLDEN_PRINT=1` prints the hashes instead of checking.

- [ ] **Step 1: Write the section.** Six cases, each rendered with `per_pixel_lighting` false and true (12 captures): (a) channel 0 colour and alpha controls equal, distance attenuation (`1|2|(1<<2)|(2<<7)|(3<<9)` in `channels[0]` and `channels[2]`); (b) alpha control differs (`channels[2]=2|(1<<2)|(1<<7)`); (c) channel 0 lit, channel 1 unlit; (d) channel 1 lit, channel 0 unlit; (e) spot attenuation (`1<<9`), two lights (`(3<<2)`); (f) unlit baked colour. Assert `hash(case, ppl)==kPplGolden[2*case+ppl]`.
- [ ] **Step 2:** Build `renderer_effects_smoke`, run `UC_GOLDEN_PRINT=1 renderer_effects_smoke --ppl-exact` on the unchanged shaders, paste the 12 hashes into `kPplGolden`.
- [ ] **Step 3:** Run `renderer_effects_smoke --ppl-exact --ppl` → PASS (characterisation: green before any shader change). Note in a comment that the hashes are GPU-specific and recorded on the owner's PC.

### Task 3: One light loop per channel

**Files:**
- Modify: `renderer/shaders/smoke.slang` (`shadeChannel`, lines 99-104)

**Interfaces:**
- Consumes: `lightControl(float4 base, uint channel, uint control, float3 position, float3 normal)`; Task 2 hashes.
- Produces: `shadeChannel` keeps its signature `float4 shadeChannel(float4 base, uint channel, float3 position, float3 normal)`.

- [ ] **Step 1:** Change `shadeChannel` so `lightControl` runs once with `channels[channel]`; when `channels[channel+2] != channels[channel]` run it a second time and take only `.a`. (Equal controls give equal alpha lanes: exact.)
- [ ] **Step 2:** Rebuild the shaders and `renderer_effects_smoke`; `renderer_effects_smoke --ppl-exact --ppl --blur` → PASS with the unchanged `kPplGolden`.
- [ ] **Step 3:** Run the D3D smokes (`native_renderer_smoke`, `native_game_renderer_smoke`) → PASS (same `.slang` feeds D3D11/D3D12).

### Task 4: No vertex lighting where the pixel stage redoes it

**Files:**
- Modify: `renderer/shaders/smoke.slang` (`vertexMain`, lines 154-160)

**Interfaces:**
- Consumes: `projection.effects & 1u` (per-pixel flag, set at `vulkan_renderer.cpp:2282`, `d3d12_renderer.cpp:1659`, `d3d11_renderer.cpp:725`); fragment condition at `smoke.slang:267-270`.

- [ ] **Step 1:** In `vertexMain`, for channel *c* in {0,1}: if `(projection.effects&1u)!=0` and `((drawState.channels[c]|drawState.channels[c+2])&2u)!=0` (exactly the fragment stage's condition), set `output.color/color1 = input.color/color1` and skip `shadeChannel`; otherwise unchanged. Guard with `#if defined(MOTION) || defined(UC_DIRECTX)` as the fragment code is.
- [ ] **Step 2:** `renderer_effects_smoke --ppl-exact --ppl` → PASS (cases (c) and (d) cover the per-channel condition); D3D smokes → PASS.

### Task 5: Light loop over set bits only

**Files:**
- Modify: `renderer/shaders/smoke.slang` (`lightControl`, lines 71-95)

- [ ] **Step 1:** Replace `for(i<8) if(mask&(1<<i))` with iteration over the set bits of `mask` (`firstbitlow`, clear lowest bit). Do not change any arithmetic; the golden hashes catch a rounding change.
- [ ] **Step 2:** `--ppl-exact --ppl` and D3D smokes → PASS (the measurement is Task 6).

### Task 6: Checkpoint 1 — measure Tasks 3-5 and test the critical-path hypothesis

**Files:**
- Create: `docs/spikes/P1-effects-results.md`

- [ ] **Step 1:** Cross-build (`pwsh scripts/linux/build.ps1 -Target uc`), upload the baseline (`~/uc-run/uc`, copy to `uc_base`) and the new binary as `uc_p1`; Deck free. Run `deck_perf.py --owner uc_base uc_p1 uc_base uc_p1` and the same without `--owner` (defaults).
- [ ] **Step 2:** Write the results table (Max, mean, lowest, p99, worst, `[gpu-sum]` per category) for both settings into the spike document, plus a Windows live `perf` run at the owner's settings (Vulkan) for the same two builds.
- [ ] **Step 3: Gate.** If the owner's-settings gain is under 1 FPS although `scene` GPU time fell by at least 60 ms/s, the critical-path hypothesis is false: write that in the document, do not start Tasks 7-9, and ask the owner how to continue. Otherwise continue.
- [ ] **Step 4:** Delete `uc_base`, `uc_p1` and run leftovers from the Deck.

### Task 7: Motion blur sample count follows the blur length

**Files:**
- Modify: `renderer/shaders/motion_blur.slang` (`blurMain`), `renderer/motion_blur_pass.cpp` (`record`), `renderer/d3d12_renderer.cpp:816` (`NativeBlurParams`), `renderer/d3d11_renderer.cpp` (`blur_scene`, same params), `tests/renderer_effects_smoke.cpp` (section `blur-adaptive`)

**Interfaces:**
- Produces: `Params.reserved != 0` means "use the full preset sample count" (debug); env `UC_BLUR_FULL_SAMPLES=1` sets it in all three backends, read per record call.

- [ ] **Step 1: Write the failing test** `blur-adaptive`: render the moved red quad of section `blur` twice, once with `UC_BLUR_FULL_SAMPLES=1`; assert `maxChannelDiff(adaptive, full) <= 8`, `distinct(adaptive) >= 8`, and the still-scene output equals the unblurred frame. Write both captures with the png writer in `renderer/png.cpp` to `build/blur-adaptive-full.png` and `build/blur-adaptive-adaptive.png`; print the max diff.
- [ ] **Step 2:** Add to the test `CHECK(renderer.blur_samples_last() < preset_samples)` for the adaptive render and `== preset_samples` for the `UC_BLUR_FULL_SAMPLES=1` render (`uint32_t blur_samples_last() const` on the Vulkan renderer, set in `MotionBlurPass::record` to the count it expects the shader to use for the longest vector, `preset.samples` when `reserved != 0`). Run → FAIL (accessor missing, then adaptive count not yet implemented).
- [ ] **Step 3:** In `blurMain`, after `length` is known: `count = params.reserved != 0 ? params.samples : min(params.samples, max(4u, uint(ceil(length))))`, loop to `count`. Set `reserved` from the env in `MotionBlurPass::record` and the two D3D param structs.
- [ ] **Step 4:** Test PASS in Vulkan; `native_renderer_smoke` PASS in D3D12 and D3D11.
- [ ] **Step 5: Owner checkpoint.** Show the two PNGs and the printed max diff; keep the change only if the owner accepts. If not, revert the loop-count line and keep only the env switch removed.

### Task 8: Motion blur copy and barriers (Vulkan)

**Files:**
- Modify: `renderer/motion_blur_pass.cpp:116-131` (`record`)

- [ ] **Step 1:** Limit `VkImageCopy.extent` to `{output_size.width, output_size.height, 1}` (the shader reads `sourceColor` only inside `outputSize`); replace the two inner `barrier_all` calls with precise ones: transfer-write → compute-read after the copy, compute-write → compute-read between `tileMain` and `blurMain`. Keep the entry and final `barrier_all` (other passes depend on them).
- [ ] **Step 2:** `renderer_effects_smoke --blur --blur-adaptive` → PASS with identical pixels to before (compare against `UC_BLUR_FULL_SAMPLES=1` captures from Task 7 saved earlier: assert byte equality). Run with `--validation` once: no sync errors.

### Task 9: Motion blur copies (D3D12, D3D11)

**Files:**
- Modify: `renderer/d3d12_renderer.cpp:802-820` (`motion_blur`), `renderer/d3d11_renderer.cpp` (`blur_scene`, ~line 1216-1234)

- [ ] **Step 1 (D3D12):** Copy `efb_` into `effect_source_` only for the `width×height` region (`CopyTextureRegion`), drop the second `CopyResource` into `effect_output_`, and copy `effect_output_` back to `efb_` for that region only; pixels outside the scene region are untouched in `efb_`, so output is unchanged.
- [ ] **Step 2 (D3D11):** same region-limited copies (`CopySubresourceRegion` with the `D3D11_BOX` already used at line 1234).
- [ ] **Step 3:** `native_renderer_smoke` and `native_game_renderer_smoke` PASS; add to the D3D smoke a case that toggles motion blur off/on and changes the render scale mid-run, asserting the post-toggle capture equals a fresh-start capture (Review Focus: stale images).

### Task 10: EFB copies and EFB draws — what are they for?

**Files:**
- Modify: `renderer/vulkan_renderer.cpp` (around `record_texture_copy`, line 474, and the timed copy at 2426)
- Create: `docs/spikes/P1-efb-copies.md`

- [ ] **Step 1:** Under `--profile`, log once per second `[efb-copies] n <count> full <count of copies >= 90% of the EFB> pixels <sum> sampled <count whose target is bound as a texture in a later draw before the next clear>`; "sampled" needs a flag on the copy target set where textures are bound.
- [ ] **Step 2:** Run the first scene and `fight4` replay on Windows and the first scene on the Deck; put the numbers and per-category GPU time in the spike document with a list of candidate eliminations (unsampled copies, full-size copies with a small reader).
- [ ] **Step 3:** Stop. Candidates go to the owner as a separate decision; nothing is removed in this task.

### Task 11: Bloom small levels (gated)

**Files:**
- Modify: `renderer/bloom_pass.cpp`, `renderer/shaders/bloom.slang`, `renderer/d3d12_renderer.cpp`/`d3d11_renderer.cpp` bloom paths

- [ ] **Step 1:** Using the `bloom` timer and `[gpu] passes`, measure the two smallest levels' dispatches plus barriers on the Deck (`pass_ms(GpuPass::bloom)` before/after a debug build that skips levels 4-5; no picture requirement).
- [ ] **Step 2: Gate.** If those dispatches are under 0.10 ms per frame, record that in `P1-effects-results.md` and stop. Otherwise fold levels 4 and 5 into one dispatch with the same arithmetic order as `bloom_math.h`, add a case to `tests/test_bloom_math.cpp` comparing the folded result to the unfolded on a fixed pyramid byte for byte, and mirror it in the D3D bloom path.

---

## Part B: where the guest thread's time really goes

### Task 12: Stack samples on Linux and an inclusive-time script

**Files:**
- Modify: `game/cpu_sampler.cpp` (Linux branch, `on_profile`, `write_samples`)
- Create: `scripts/inclusive_profile.py`, `scripts/test_inclusive_profile.py`

**Interfaces:**
- Produces: with `UC_CPU_SAMPLE=1 UC_CPU_SAMPLE_STACKS=1`, the output file gains lines `s <tid> <ms> <rip> <ret1> <ret2> ...` (up to 12 return addresses, hex). `inclusive_profile.py <samples> <binary> [top]` prints `incl% self% symbol` for the guest thread, one row per function.
- Env unset ⇒ code path and cost identical to today.

- [ ] **Step 1: Write the failing test** `test_inclusive_profile.InclusiveTest.test_counts_each_function_once_per_sample`: a synthetic file with 3 samples of the stack `A <- B <- C` and 1 of `A <- B`, a fake symbolizer mapping addresses to `A,B,C` (stack `A <- B <- C`: the instruction pointer is in A, called by B, called by C); with 3 samples of `A <- B <- C` and 1 of `A <- B` assert `incl(A)==1.0`, `incl(B)==1.0`, `incl(C)==0.75`, `self(A)==1.0`, and that a recursive sample `A <- A` counts `A` once.
- [ ] **Step 2:** `py -3.13 -m unittest test_inclusive_profile` from `scripts/` → FAIL.
- [ ] **Step 3:** Implement the script (resolve with `llvm-symbolizer` as `resolve_linux_samples.py` does; dedupe symbols per sample).
- [ ] **Step 4:** In `on_profile`, when stacks are enabled, call `backtrace()` into a second ring `StackSample {tid, ms, depth, frames[12]}` (2^16 entries) and write the `s` lines; call `backtrace` once at startup outside the handler so its lazy init does not allocate in the handler.
- [ ] **Step 5:** Cross-build, run the first scene on the Deck with `UC_CPU_SAMPLE=1 UC_CPU_SAMPLE_STACKS=1 UC_CPU_SAMPLE_SECONDS=9`, fetch the file, run the script. Expected: `fn_`/`body_` rows with inclusive% ≥ self%. Clean the Deck.

### Task 13: Inclusive profile of three scenes (research output)

**Files:**
- Create: `docs/spikes/P2-guest-inclusive-profile.md`
- Modify: `project/symbols.txt` (names of identified loops, format `<addr> <name> manual <evidence>`)

- [ ] **Step 1:** Profile the first scene (Deck, 9 s window), the fight (`fight4` replay window on Windows `build-prof` with the existing 10-frame sampler and on the Deck with Task 12's), and a room scene (`uctest run` with a scenario that reaches one; reuse `perf` with a different chapter).
- [ ] **Step 2:** Table: top 30 guest functions by inclusive share between 1% and 20% of the guest thread, with calls per frame (build `UC_FN_COUNT` once, `fn_count` output) and iterations per call where a loop is evident.
- [ ] **Step 3:** Read the translated source of each (`generated/generation_549e9dfe8a254cc8/game_NNN.cpp`), name it by behaviour (actor update, skinning, particles, collision, animation) and add a symbols line with the evidence.
- [ ] **Step 4: Checkpoint 2.** Send the table to the owner with the candidate micro-wins (Task 14) and loops (Task 15); wait for the choice.

### Task 14: Micro-wins picked from the profile (repeat per function)

**Files:**
- Modify: the file holding each chosen function (HLE leaf in `runtime/hle/leaf_gen.cpp`/`sdk_math.cpp`, or a native replacement registered like `hle_Leaf_*`)
- Test: a differential test beside the existing ones (pattern: `tests/test_normal_matrix_hle.cpp`, `test_gx_transforms.cpp`)

- [ ] **Step 1:** For one function from the P2 table with inclusive ≥ 1% on both platforms: write the differential test feeding ≥ 10 000 recorded or generated inputs to the translated function and the native version, asserting identical guest-memory and register results.
- [ ] **Step 2:** Run → FAIL (native version missing). Implement; run → PASS.
- [ ] **Step 3:** `py -3.13 -m uctest demo check fight4` (bit-identical replay) and `uctest bench <before> <after> --demo fight4 --runs 3` on Windows; `deck_perf.py --owner` alternating on the Deck. Keep when not slower at both; drop when under 1% on both.
- [ ] **Step 4:** Repeat Steps 1-3 for the next function; stop after the first function that gains nothing measurable twice in a row.

---

## Part C: parallel guest loops (only after Checkpoint 2)

### Task 15: Memory-access tracer

**Files:**
- Create: `runtime/mem_trace.h`, `runtime/mem_trace.cpp`, `tests/test_mem_trace.cpp`, `scripts/loop_independence.py`
- Modify: `runtime/ppc_rt.h` (every accessor that calls `hp()`: `rd8/16/32`, `rdf32/64`, `wr8/16/32`, `wrf32/64`, the `_ram`/`_stack` variants, load/store-multiple, `psq`, `dcbz`), `runtime/CMakeLists.txt`, `game/CMakeLists.txt` (option `UC_TRACE_MEM`, default OFF)

**Interfaces:**
- Produces: `uc::mem_trace::begin_region(uint32_t function, uint32_t iteration)`, `end_region()`, `note_read(uint32_t ea, uint32_t size)`, `note_write(uint32_t ea, uint32_t size)` (all no-ops compiled out unless `UC_TRACE_MEM`); `UC_TRACE_MEM_OUT=<file>` receives one binary record per iteration (function, iteration, merged read ranges, merged write ranges). `loop_independence.py <file> <function>` prints `independent`, `independent+reduction (addr list)` or `dependent (first conflicting pair)`.

- [ ] **Step 1: Write the failing tests.** C++ `mem_trace_merges_ranges`: two adjacent 4-byte writes → one 8-byte range; `mem_trace_off_is_free`: with the macro off `note_write` is an empty inline (static_assert on `std::is_empty` of the state type). Python `test_loop_independence`: three synthetic iterations writing disjoint ranges → `independent`; iteration 2 reading what iteration 1 wrote → `dependent` naming both iterations; a shared accumulator written by all → `independent+reduction`.
- [ ] **Step 2:** Run both → FAIL. Implement the tracer and script.
- [ ] **Step 3:** Add a script test `scripts/test_hp_coverage.py` that fails if `ppc_rt.h` calls `hp(` anywhere outside a function that also contains `mem_trace::note_`; run → PASS.
- [ ] **Step 4:** Build with `-DUC_TRACE_MEM=ON` (Windows `build-prof`-style folder), confirm the normal build's `uc.exe` is unchanged in size and `uctest demo check fight4` still passes on the normal build.

### Task 16: Independence proof for the Checkpoint 2 candidates

**Files:**
- Modify: the generated-code entry hooks for the chosen loops through `game/entry_probe.h` (the `UC_ENTER` macro: `begin_region` on loop entry, `end_region` per iteration — add a `constexpr` list of candidate addresses next to `UC_FN_COUNT_HOOK`)
- Create: `docs/spikes/P3-loop-independence.md`

- [ ] **Step 1:** For each owner-approved candidate, run the tracer on the first scene, `fight4` and a room scene; run `loop_independence.py`. Also grep the loop body in generated code for calls into GX/OS/scheduler HLE (`safe_point`, `OSDisableInterrupts`, `gx_fifo_write`) and for global scratch writes (netplay lesson: scratch at `80703FA0`).
- [ ] **Step 2:** Table in `P3-loop-independence.md`: loop, share of guest thread (from P2), iterations per call, verdict, evidence. Only `independent` and `independent+reduction` with ≥ 100 µs per call go on.
- [ ] **Step 3: Checkpoint 3.** Present the table and the Amdahl estimate (share × (1 − 1/workers)); the owner decides whether Task 17-18 are worth doing.

### Task 17: Worker pool with a serial path

**Files:**
- Create: `runtime/par_for.h`, `runtime/par_for.cpp`, `tests/test_par_for.cpp`
- Modify: `runtime/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `uc::par::init(unsigned workers, std::span<const unsigned> cpus)` (workers 0 = serial), `uc::par::shutdown()`, `template<class F> void uc::par::for_range(uint32_t begin, uint32_t end, uint32_t grain, F&& body)` where `body(uint32_t lo, uint32_t hi)` runs on any thread and the call returns after all ranges finished; `unsigned uc::par::worker_count()`.

- [ ] **Step 1: Write failing tests:** `for_range_covers_every_index_once` (10 000 indices, 3 workers, atomic counters each equal 1); `serial_when_no_workers` (workers 0 → body runs on the calling thread, ranges ascending); `exception_in_body_rethrown_after_join`; `repeated_calls_do_not_leak_threads` (1000 calls, thread count constant).
- [ ] **Step 2:** Run → FAIL. Implement: workers spin ≤ 20 µs on an atomic generation counter, then park on a condition variable; the caller takes ranges too; pin workers to `cpus`.
- [ ] **Step 3:** Run → PASS. Choose `cpus` at game start: Deck = the physical cores other than the guest's pinned core and its SMT sibling; Windows = from the launcher calibration the guest-pin already uses (`launcher/cpu_calibration.cpp`, `--guest-cpu`); a machine with ≤ 2 cores gets `workers = 0`.

### Task 18: Convert one proven loop (repeat per approved loop)

**Files:**
- Modify: the HLE file registering a native version of the chosen function (same pattern as the existing `hle_Leaf_*`)
- Test: `tests/test_par_<loop>.cpp`

- [ ] **Step 1: Write the failing test:** run the serial reference and the parallel version on three recorded states of the loop; assert the written guest memory ranges and result registers are byte-identical, and that `par::init(0, {})` gives the serial result too.
- [ ] **Step 2:** Run → FAIL. Implement the native loop with `par::for_range`, keeping the serial function in the build as `<name>_serial` (used when `worker_count()==0`).
- [ ] **Step 3:** Run → PASS; then `uctest demo check fight4` (bit-identical), a 2-minute netplay desync run (`test-multiplayer.bat`, state hashes equal), `uctest bench` on Windows (3 rounds) and `deck_perf.py --owner` alternating on the Deck.
- [ ] **Step 4: Gate.** Keep only if the guest-thread gain is ≥ 3% on the Deck; otherwise remove the parallel version, keep the serial one, and stop Part C.

### Task 19: Close-out

- [ ] **Step 1:** Full `build.bat` (tests pass), deploy to the game folder and the Deck `uc`; clean test leftovers from the staged folder first.
- [ ] **Step 2:** Final review of the branch on Opus (`model: "opus"`); fix findings.
- [ ] **Step 3:** Update memory notes (`effects-cost-and-roadmap-2026-10-30.md`, `steamdeck-perf-findings.md`) with the measured results and the proposed commit messages for the owner.
