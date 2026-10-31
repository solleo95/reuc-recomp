# Performance roadmap: effects on the GPU, small exact wins, parallel guest loops

Date: 2026-10-30. Branch: feature/linux-steamdeck. Everything here must help Windows and the Steam Deck; Deck-only or Windows-only work is marked. Decisions taken by the assistant are marked *call*.

## Intent

The owner plays the Deck with per-pixel lighting, ultra motion blur and bloom 150 (config: `fps_limit 90`) and gets 67-70 FPS where the default settings give 74-77. Past work (GX display-list recording, bloom, depth barrier, PGO, znver2, core pin) is shipped. Three directions remain and the owner chose all three:

1. **Cost of the effects** (per-pixel lighting, motion blur, bloom) on the GPU.
2. **Small exact wins** in translated and HLE code ("no big target left, 1-2% per function, strict equality checks").
3. **Parallel guest loops**: independent loops of the game (actor update, skinning, particles, collision) run on several cores as native functions, after a proof that they share no written memory.

## What the measurements say (Deck, first scene, 10 s, unlocked, one run each, noise 2-3%)

| Setting | FPS mean (max) | GPU ms/s: scene / efb / copies / bloom / blur | GPU busy total | render thread |
|---|---|---|---|---|
| defaults | 76.8 (82) | 301 / 112 / 84 / 42 / 0 | 545 | 413 |
| + per-pixel lighting | 73.9 (77) | 427 / 95 / 78 / 42 / 0 | 649 | 403 |
| + motion blur ultra | 77.1 (80) | 302 / 95 / 74 / 41 / 98 | 617 | 475 |
| + bloom 150 | 76.7 (81) | 300 / 97 / 80 / 43 / 0 | 527 | 403 |
| all three (the owner's) | 68.6 (73) | 395 / 91 / 69 / 37 / 89 | 687 | 455 |

Reading:

- Per-pixel lighting adds **+126 ms/s of scene draws (+42%)** for a 3-FPS loss. Motion blur adds 98 ms/s GPU and about 60 ms/s on the render thread for **no** FPS loss alone. Bloom strength costs nothing.
- Together they cost **8 FPS**, more than the sum of the parts. Hypothesis (to confirm in phase 1): below roughly 60-65% GPU busy the GPU work overlaps the guest thread and is hidden; above it the GPU is on the critical path and every GPU millisecond costs frame time (all three: +142 ms/s of GPU gives +1.56 ms of frame time). If true, **cutting 100 ms/s of GPU time is worth about 4-6 FPS at the owner's settings and about 0 at the defaults**.
- Always-present GPU cost, independent of the effects: EFB draws 90-112 and texture copies 70-84 ms/s (the game's ~23 EFB copies per frame), bloom 40.
- The guest thread is still busy 85-90% and its profile is flat (translated code 42% over hundreds of functions, largest 1.7%; GX 14.5%; `hle_Leaf_*` 11%; other HLE 11%; libc 6.7%).

Code facts found while reading:

- `renderer/shaders/smoke.slang` `shadeChannel` runs the whole GX light loop twice per channel (colour with `channels[c]`, alpha with `channels[c+2]`); with per-pixel lighting this runs in the fragment shader for channels 0 and 1, so up to four light loops per pixel. The vertex stage also computes the lighting that the fragment stage then redoes.
- `renderer/shaders/motion_blur.slang` gathers a fixed `samples` count per pixel, each sample with a `motionAt` (two integer divisions and a load) plus a colour load; `motion_blur_pass.cpp` makes a full-frame image copy and five `ALL_COMMANDS` barriers per frame.
- `bloom_pass.cpp` already uses precise compute barriers; what is left is the chain of 12 small dependent dispatches.
- Shaders (`.slang`) are shared by Vulkan, D3D12 and D3D11 (Windows default is Vulkan, `options.h`); the passes themselves (motion blur, bloom) exist once per backend (`motion_blur_pass.cpp`, `d3d12_renderer.cpp`, `d3d11_renderer.cpp`).
- The game code is anonymous: `project/symbols.txt` has 28 names. Finding loops needs call-tree profiling, not names. The Linux sampler records only the instruction pointer; the Windows sampler (`UC_CPU_SAMPLE`, `build-prof`) records 10-frame stacks.

## Goals

- Raise FPS at the owner's settings on the Deck by shortening the GPU critical path, without changing the picture beyond a stated, bounded difference.
- Keep equality exact for every guest-code change (differential tests plus the guest-memory hash check of `uctest demo check`).
- Find, with a measurement and not a guess, which guest loops are independent and how much of the guest thread they hold; build workers only for loops where the bound is worth it.
- Every change runs on Windows (Vulkan, D3D12, D3D11 for passes; shaders shared) and the Deck, or is explicitly marked as one of them.

## Non-goals / anti-goals

- No automatic multithreading of guest threads (they rely on one-CPU semantics: interrupt masks as locks).
- No change of GPU clocks, default render scale or default effect settings.
- No picture change for the default quality unless a before/after image diff is shown to the owner first.
- A parallel loop that cannot be proven independent is not built; a result that is "technically faster" but desyncs netplay or demos is a failure.
- No thread offload of GX (measured: at most 3% left, 2026-10-30).

## Measurement protocol (all phases)

- Settings: the owner's (`--per-pixel-lighting --motion-blur ultra --bloom-strength 150 --fps-limit 90`) **and** the defaults, because the GPU is on the critical path only in the first.
- Deck: `scripts/linux/deck_perf.py` in Game Mode, builds alternated, 2 runs each at least; report Max FPS, mean, lowest second, p99 and worst frame from `[frametime]`, and `[gpu-sum]`/`[gpu]` per category (`pacing.py`/`gpusum.py` style summaries). The game on the Deck must be closed: check `ssh deck pgrep -a uc` first.
- Windows: deterministic demo bench (`uctest bench --demo fight4`, run with Python 3.13, game exe from the game folder) for CPU-side changes; for GPU-side changes the `[gpu-sum]` lines of a replay and a live `perf` run at the owner's settings. Windows has the D3D12 and D3D11 backends: each pass change is checked in all three through the existing GPU smoke tests, a screenshot diff and the pass timers.
- A change is kept when it is correct and not slower at both settings (owner rule: keep small wins), and the report says which of Max/mean/p99 moved.

## Phase 1: effects (GPU), shared shader work first

Order is by expected gain per risk. Each item: one change, measured alone.

1. **Light loop once per channel (`smoke.slang`).** When `channels[c]` and `channels[c+2]` select the same lighting model, take alpha from the colour evaluation; when they differ, evaluate only the alpha lane. Exact (same arithmetic), shared by all three backends. Expected: a large part of the +126 ms/s.
2. **No vertex lighting when the pixel stage redoes it.** A per-pixel-lighting shader variant skips `shadeChannel` in `vertexMain` for channels that the fragment stage recomputes (pipeline key bit). Exact.
3. **Cheaper light loop.** Hoist per-light constants out of the pixel loop (loop over `mask` bits only, `rsqrt` instead of `sqrt` + divide where the GX equation allows bit-exact results, packed light structs). Must stay bit-exact in the `int` rounding steps; verified against the vertex-lit result on a still scene and the existing lighting tests.
4. **Motion blur samples follow the blur length.** `samples = min(preset, ceil(length / k))` and the motion vector fetched once per sample position only when render and output sizes differ. Picture change: bounded, shown as an image diff plus a flicker check on a camera pan; if the owner wants bit-identical, only the `motionAt` hoisting stays.
5. **Motion blur plumbing (`motion_blur_pass.cpp`, D3D12, D3D11).** Replace the full-frame copy plus `ALL_COMMANDS` barriers with a ping-pong target and compute-only barriers; same change in the two Windows backends. Also measures the extra 60 ms/s on the render thread that motion blur adds.
6. **Bloom chain.** Fold the smallest levels (20x11 and 40x22 in the Deck window) into one dispatch (single-pass downsample style), fewer dependent barriers. Exact result for the folded levels required (same arithmetic order).
7. **Always-present GPU cost: EFB draws and texture copies (90-112 and 70-84 ms/s).** First an investigation: timestamps per copy, which of the ~23 copies per frame are full-size, which have a reader. Candidates: skip copies nobody samples, copy at the size the reader uses, merge barriers. Output of the investigation is a list; only items with exact output are built.
8. **Confirm the critical-path hypothesis** by plotting FPS against `[gpu-sum]` total over all five configurations plus the new builds. The plan below the bullets assumes it holds; if it does not, phase 1 is reordered by measured FPS, not by GPU ms.

Exit: owner's settings at least +4 FPS on the Deck (stretch: back within 3 FPS of the defaults), no loss at the defaults, all three backends build and pass the GPU smoke tests.

## Phase 2: small exact wins in code the guest thread runs

Known leftovers from the profile (each 0.5-2%): `hle_Leaf_80394374` (2.5%), `PSMTXConcat` (1.4%), the largest translated functions (1.7% each), `gx::share` memcmp (about 1%), `TextureCache::snapshot`, `GXBegin`, `Packets::seal_batch`, DrawPacket assembly per mesh draw (many `shared_ptr` copies).

Method per item: profile the function with a line-level sample (compile the file with `-gline-tables-only`), write the native version or the cheaper path, prove equality (differential test against the translated function over recorded inputs, or the hash check), measure with `uctest bench --demo` on Windows and `deck_perf` on the Deck. Items below 1% on both platforms are dropped, not stacked. Results of the 2026-10-30 trial: the transform-memcmp skip and ThinLTO showed nothing outside noise.

## Phase 3: parallel guest loops (research first, build only with proof)

### 3a. Find the loops (no product change)

- Add stack unwinding to the Linux sampler (frame-pointer walk of the guest thread) so both platforms give **inclusive** time per guest function. On Windows use `build-prof`.
- Record the first scene, the fight (`fight4`) and a room scene; aggregate by guest function address; list the top 30 inclusive functions between the frame loop and the 1% leaves, with iteration counts per call (call counts exist as the `FN_COUNT` probe).
- Name them by behaviour (actor update, skinning, particles, collision, animation) from reading the translated code; write them into `project/symbols.txt`.
- Output: a table of candidate loops with the share of the guest thread, the typical iteration count and the cost per iteration. A loop is interesting only if its cost per call is above about 100 microseconds (a worker handoff costs 5-30 microseconds on the Deck; `pthread_cond_signal` was already 0.4-2.4% of a sample).

### 3b. Prove independence

- Add a build-time tracer (`UC_TRACE_MEM`) that records, per loop iteration, the guest address ranges read and written (the memory accessors already pass through `hp()` and the write barrier `gx_write_watch.h` stamps pages on stores).
- For each candidate loop, run the three recordings and check: writes of iteration *i* are disjoint from reads and writes of every other iteration; no call inside the loop to GX/OS/scheduler HLE (`safe_point`, `OSDisableInterrupts`, GX FIFO); no use of global scratch (the netplay desync of 2026 was a shared scratch buffer at 80703FA0, the same class of bug).
- Output per loop: independent / independent with a reduction (sum, max) / not independent, with the evidence.

### 3c. Build a worker pool and one loop

- Pool: workers pinned to the other physical cores, spinning for a short time then parking, a per-job `Ctx` copy (registers are per thread, memory is shared), one fork/join per loop call. Deck: 4 cores, guest pinned to the second physical core already; Windows: cores from the existing launcher calibration. Worker count adapts to the machine; one core machines fall back to the serial path.
- Each parallel loop gets a **serial reference path** that stays in the build; a test runs both on recorded inputs and compares the written memory byte for byte; the demo hash check (`uctest demo check`) and a netplay desync run gate the merge.
- Loop order: the one with the best (share x scalability) from 3a/3b first. Stop after the first loop if its measured gain is under 3% of the guest thread on the Deck.
- Amdahl bound, given honestly: if the proven-independent loops total 20% of the guest thread and scale on 3 workers, the gain is about 13% on the Deck; if they total 10%, about 6%. The decision to build more than one loop is taken with the measured numbers of 3a-3c, not before.

## Order, dependencies, checkpoints

1. Phase 1 items 1-3 (shader, shared by all backends), measure; items 4-6 (passes in three backends); item 7 investigation. Checkpoint with the owner: the numbers table and image diffs.
2. Phase 3a runs in parallel with phase 1 (profiling work, no shared files with the shader work).
3. Phase 2 items are picked from the 3a inclusive profile (it shows what is really worth 1-2%).
4. Phase 3b/3c after the 3a table; checkpoint with the owner before building a pool.

## Risks

- The critical-path hypothesis may be wrong: then GPU savings pay less than predicted (phase 1 item 8 settles it early).
- Shader changes touch every lit draw: any inexact change shows as a lighting difference; every step is checked against the previous output on a still scene.
- D3D12/D3D11 passes are written separately: parity bugs are likely; the smoke tests and a screenshot diff per backend are required, not optional.
- Parallel loops: hidden dependencies through globals, FP rounding if a reduction changes order, and wake-up latency larger than the loop. Proof comes before the pool; a serial fallback stays.
- Measurement noise is 2-3% on the Deck: only differences repeated in alternating runs count; no single run decides.

## Decisions taken without asking (*call*)

- Phase order: effects first because the owner's settings make the GPU the limiter, and it is the lowest-risk work.
- Motion blur sample reduction is a bounded picture change and is shown before it is kept.
- The tracer and the sampler stacks are tooling and may stay in the tree behind build flags.
- Doc language: English like the other specs; chat reports go to the owner in Russian.
