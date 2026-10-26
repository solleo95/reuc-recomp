# Guest frame cost and packet pipeline (2026-10-15)

Follow-up to F2 (`F2-native-240-feasibility.md`): make the 60 Hz step (`--unlock-60fps`) stable with headroom.

## Findings (RTX 4080, train gameplay, 1280x720 window, 2x scale)

- A game frame is ~23 packets (one per EFB copy, ~850 draws), not 2.4. The main loop rendered one packet per iteration and then waited on a fence, and the guest could have only 2 packets in flight, so the guest stalled ~30% of every frame on the renderer. Render scale 0.5x and 2x gave the same FPS: not GPU bound (GPU busy ~3 ms/frame, render-thread CPU ~4 ms/frame).
- Guest-thread cost per frame was ~20 ms: `DrawPacket` (~6 KB) moves on vector growth, whole-vertex copies of cached meshes per draw, `RecordedDraw` (~5 KB) moves inside `vector<variant>` growth.

## Changes

- `FramePacket::draws` and the import lists are `std::deque` (no relocation); `hle_ResMeshDraw` appends in place and rolls back on failure.
- Cached meshes share `gx::MeshBuffers` through `DrawPacket::mesh`; consumers read `vertex_view()` / `index_view()`.
- `Resource` stores `RecordedDraw` by `unique_ptr`.
- `Packets`: window of 256 unfinished packets (64 still stalled 4 ms/frame) and at most 2 presenting frames in flight; renderer slots are a separate constant (6).
- Main loop drains all ready packets per iteration and does not wait while more are queued.
- `perf` scenario: menus at 8x, measurement at speed 1.

## Result

`--unlock-60fps`: stable 60 presentations/s in the train (was 47-51); guest work ~8.5 ms/frame, ~8 ms idle per frame at 60 Hz. 97/97 tests pass.

## Diagnostics added

`[perf]`/`[guest-blocked]` (renderer wait split into window/frames, GPU busy from timestamps, frame latency), `[game-pacing] ... wait` (guest idle in the frame wait = headroom). `UC_CPU_SAMPLE=guest|main` (+`UC_CPU_SAMPLE_DELAY=<s>`) samples a thread with 10-frame stacks into `cpu-samples.txt`; resolve with `scripts/resolve_cpu_samples.py` against a `/Z7` build (`build-prof`: configure with `CMAKE_CXX_FLAGS_RELEASE="/O2 /Ob2 /DNDEBUG /Z7"` and `CMAKE_EXE_LINKER_FLAGS="/machine:x64 /DEBUG /OPT:REF"`).

## Remaining guest hot spots

`hle_ResMeshDraw` per-draw `State`/`Transforms`/`DrawUniform` copies (~9%), `TextureCache::snapshot` whole-texture compares (~5%), `operator new` (~4.5%). Next: presentation at 240 Hz (interpolation), pipeline warm-up, present pacing.

## Above 60 Hz: `--game-hz N` (61..480, experimental)

`hle_GameFrameWait` waits for a wall-clock deadline (virtual clock, coarse scheduler sleep + 2 ms spin) instead of a VI retrace and feeds the real elapsed time as the timestep (`object+4/+8`, counter `+0x20` = 1). VI and audio stay at 59.94 Hz. A late frame starts the next one at once (no slot skipping, which halved the rate near the limit).

| Requested | Presents/s | Game seconds per second | Guest idle per frame |
|---|---|---|---|
| 90 | 90 | 1.000 | 2.6 ms |
| 100 | 98-100 | 1.000 | 1.5 ms |
| 110 | 110 | 1.000-1.004 | 0.8 ms |
| 120 | 117-118 | 1.000 | 0.2 ms |
| 144 / 165 (uncapped) | 121-123 | 1.000 | 0 ms |

Ceiling is ~123 Hz (guest ~8.1 ms/frame). Owner watched 100 FPS gameplay: looked correct. `controls` passes at 90 Hz; 90 s autoplay with `--god` at 90 Hz: 28 kills, 0 deaths, no crash. Animation/physics/event parity at other rates is not systematically verified.

Uncapped profile (guest thread): `TextureCache::snapshot` whole-texture compare ~10% (memory bound even with memcmp; exact validation kept), `operator new` ~5% (one allocation per `DrawPacket` in the deque), `hle_ResMeshDraw` ~4%, `DrawPacket` moves ~3.5%, translated game code ~16%.

`--profile` now also prints `[frametime] n/avg/p50/p99/max` of the interval between presents. Live measurements need an unlocked desktop session: with the session locked the window presents at ~2 FPS.

## Further squeeze (same day)

Uncapped step ceiling went from ~123 Hz to ~140 Hz (frame time avg 7.2 ms, p99 8.0 ms; autoplay 90 s at 120 Hz: 31 kills, 0 deaths; screenshots unchanged).

- **Pooled `DrawPacket` nodes** (`renderer/block_pool.h`): the deque's one-6-KB-block-per-draw no longer goes back to the OS and faults in fresh pages each frame (`operator new` 5.3% -> 3-4%).
- **Shared immutable blocks** (`renderer/gx_shared.h`): `DrawPacket::state/transforms/uniforms` are `Shared<T>` (copy on write through `edit()`, reads through `->`); consecutive draws with identical bytes share one block via `share(cache, value)` (`DrawPacket` default construction and moves no longer show in the profile). A `memcpy` of the uniforms must take `&*draw.uniforms`, not `&draw.uniforms`.
- **Guest write barrier** (`runtime/gx_write_watch.h`, called from `wr8/16/32`, `wrf64`, `dcbz` and the native bulk writers in `dvd.cpp`, `locked_copy.cpp`, `gx.cpp`): each store stamps its 4 KiB page with an epoch. The texture cache, the mesh cache (description + source arrays) and the display-list check compare guest bytes only when a store touched their pages since the last comparison, plus a full comparison every 256th use as a net for native writers that miss the barrier (texture compare was ~10% of the guest thread). New native code that writes guest RAM in bulk must call `gx::note_write_range`.
- Rejected: signalling `GXDrawDone` before the GPU finishes (intro movies stop reaching the title: the game relies on the real completion).

## What limits the step now

Guest thread ~7.1 ms: translated game code ~2 ms, GX recording ~4 ms, ~1 ms waiting for the renderer in `GXWaitDrawDone` (tail of the frame). Render thread ~60% busy, 87 000 vertices per frame converted and written to host-visible memory (`gpu_vertex`, ~19% of that thread). Next structural steps: persistent GPU vertex buffers with a matrix palette (shader change), and moving GX packet building to a worker thread. 240 Hz step needs 4.2 ms per frame, so the output path to 240 Hz is interpolation.

## GPU mesh buffer follow-up (2026-10-15)

On `gpu-mesh-buffers`, shutdown now sweeps persistent mesh slabs after device retirement. A new GPU smoke case reproduced the leaked buffer/memory pair before the fix and passes afterward. Host-visible allocations prefer device-local memory for meshes, then try compatible alternatives on allocation exhaustion; a CPU test covers fallback, exhausted/incompatible types and non-memory errors.

The old GPU smoke failures came from assumptions about immediate resize and two frame slots. It now waits for the 60 ms resize debounce and exercises texture/descriptor reuse over all six slots. Full smoke passes, including lighting/normals; 98/98 CTest tests pass. With implicit third-party Vulkan layers disabled, smoke took 5.86 s normally and 12.99 s with validation on this machine. ReShade otherwise reports a loader error unrelated to renderer validation.

Harness validation: `autoplay --god --speed 8 --max-minutes 2 --game-arg=--validation`, run `build/autotest/1792083466726395200`, completed without Vulkan leaks. Autoplay dismisses tutorials; there is still no full tutorial-disable option. Controlled windows are raised once without activation or permanent topmost status. Builds stage the exe and matching shaders together.

`perf --god --measure 15 --game-arg=--game-hz --game-arg=480 --game-arg=--profile`, run `build/autotest/1792083330625705100`: active gameplay mean 126.0 FPS, frame gap p99 12.9 ms, max 46.1 ms (288 active 50 ms windows; 76 excluded). This is not an A/B comparison with the earlier 152 FPS result. Pipeline warm-up and the remaining isolated stalls are still open.

### Paired verification

Fresh main `9149b8b` versus `3a13027` plus the shutdown/allocation/window fixes above. Both Release clang-cl, same `project/pgo/uc.profdata` and generated guest sources, matching shaders beside each executable. RTX 4080, window 1280x720, explicit `--render-scale 2 --game-hz 480 --profile --god`; copied saves, same train route. Harness drives menus at 8x and measures at 1x. Validation disabled during timing. Order main/GPU/GPU/main; 20 seconds requested active gameplay per run, 388 usable 50 ms windows each after transition exclusions. No additional steady-state warm-up; these numbers include the opening gameplay workload.

| Build/run | Active FPS | Render CPU ms/present | GPU ms/present | Event gap p99/max ms |
|---|---:|---:|---:|---:|
| main / 1792084633835738400 | 120.41 | 3.883 | 2.464 | 12.5 / 15.4 |
| GPU / 1792084661834431500 | 125.72 | 2.503 | 1.678 | 10.2 / 12.2 |
| GPU / 1792084689781981300 | 125.05 | 2.489 | 1.675 | 10.9 / 44.7 |
| main / 1792084717601226100 | 121.44 | 3.832 | 2.517 | 11.3 / 16.7 |

Combined: **120.93 -> 125.39 FPS (+3.69%)**, render CPU **3.858 -> 2.496 ms/present (-35.30%)**, GPU **2.489 -> 1.676 ms/present (-32.66%)**. Reciprocal mean FPS gives **8.269 -> 7.975 ms** between presentations. CPU and GPU work overlap; their times must not be added to derive the presentation interval.

FPS uses the existing harness active-only bins. CPU/GPU figures use complete approximately one-second profile intervals wholly inside active gameplay, excluding the first 0.5 seconds: sum `[perf] render` / presents, and deltas of cumulative `[guest-blocked] GPU busy` / presents. GPU timestamps retire asynchronously, so boundary attribution is approximate. CPU/GPU totals cover 33 intervals/3972 presents on main and 32/3982 on GPU. Event gaps are harness arrival gaps, not direct GPU presentation timestamps.

Local reproducer: `python build/bench_pair.py`; raw reports, CSVs, screenshots and timestamped profile lines live under the run directories above; combined report is `build/benchmark-pair.json`. All four processes exited normally. Screenshots show the same scene; animated actors differ, so this is not a pixel-identical comparison. Repeated full CTest: 98/98 pass.

The earlier **141 -> 152 FPS**, **3.56 -> 2.19 ms CPU**, **2.10 -> 1.48 ms GPU** claims are **not reproduced** by this paired test. Their original evidence was subsequently recovered from Claude's session; see below. The optimization measurably reduces renderer work, but this paired test supports about 125 FPS in this workload. The isolated 44.7 ms event gap remains unresolved; no claim of eliminating stutter or validating every chapter.

### Recovered Claude measurement and regression check

Claude session `a940f4f0-a7c6-466d-85aa-47593d7503fd.jsonl`, 2026-10-15 15:46:16 UTC, contains the original command/result. It ran `uctest run perf --measure 12 --god --game-arg=--game-hz --game-arg=300 --game-arg=--profile`, then selected `[perf]` records with `slice(-8,-2)`. This selects six approximately one-second windows without filtering game state. CPU and GPU figures were divided by the presentation count in those same windows. The 152 figure is therefore not the harness active-gameplay mean.

| Original run | Active gameplay FPS | Mission-screen FPS (50 ms bins) | Selected six profile records, presents |
|---|---:|---:|---|
| PGO baseline `1792079136694288800` | 136.49 | 150.14 | 144,150,146,140,136,131 (mean 141.17) |
| GPU `1792079156730224800` | 137.02 | 178.33 | 154,174,170,144,137,135 (mean 152.33) |

Each original report has 228 active windows and 76 excluded windows. Its CSV explicitly contains `mission` and mixed `active+mission` intervals. Thus the original paired gameplay-only result was nearly flat (+0.39%); the larger selected-window gain includes faster mission-screen rendering. This also explains the user's observation of higher FPS while a tutorial/mission overlay is displayed.

Fresh regression comparison, original committed `3a13027` versus current fixes, same PGO/generated sources and 20-second/480 Hz protocol as above, order original/fixed/fixed/original:

| Version | Run | Active FPS | CPU render ms/present | GPU ms/present |
|---|---|---:|---:|---:|
| Original | 1792085099724900200 | 122.68 | 2.486 | 1.693 |
| Fixed | 1792085127873194300 | 124.48 | 2.542 | 1.683 |
| Fixed | 1792085155622720700 | 123.76 | 2.547 | 1.664 |
| Original | 1792085183401772600 | 120.57 | 2.470 | 1.659 |

Mean active FPS: original 121.62, fixed 124.12. No FPS regression from the follow-up fixes is reproduced. Local reproducer/report: `build/bench_original.py`, `build/benchmark-original.json`.

Repeating Claude's 300 Hz / 12-second protocol now gives main 123.68 active FPS versus current GPU 128.42; the unfiltered six-record method gives 128.67 versus 138.33. Current GPU mission-only bins average **163.89 FPS**, with a one-second profile record of **162 presents**, consistent with the user seeing 165. Current run IDs: `1792085241442566300`, `1792085261222658700`; reproducer/report: `build/reproduce_claude_perf.py`, `build/benchmark-claude-reproduction.json`.

All three copied save files hash identically between the old GPU run and the current 300 Hz run. GPU, render/output dimensions and scene match. The remaining historical difference (137.02 versus 128.42 active FPS) is real in these reports; the mixed-window calculation alone does not explain it. Rebuilding original source also produces lower current FPS, so the follow-up fixes are not supported as its cause.

The actual historical binary was then recovered from `<scratch>/uc_gpu.exe` with matching `shaders_new`. Session chronology confirms it was built after the slab fix, saved at 15:45 UTC, and used in the original pair. Copied into `build/bench-claude/game/uc.exe`; SHA-256 of both copies: `BBEB8F6674ED99ED0A441B036535FF872213832C952A75994D651D299E4CA143`. Both shader hashes also match current dist shaders.

Exact saved exe versus current exe, again 300 Hz / 12 seconds, order saved/current/current/saved:

| Binary | Run | Active FPS |
|---|---|---:|
| Saved Claude exe | 1792085709386904000 | 124.47 |
| Current exe | 1792085729240209300 | 129.65 |
| Current exe | 1792085749031406600 | 132.02 |
| Saved Claude exe | 1792085768806552900 | 130.96 |

Mean saved **127.72**, current **130.83 FPS**. Even the unchanged historical executable does not reproduce its earlier 137.02 active FPS, and varies by 6.49 FPS between these two runs. This excludes our source/build changes as a necessary cause of the historical slowdown and provides no evidence of an FPS regression from our fixes. Runtime conditions/run variation remain; exact CPU/GPU clocks and background workload at the historical measurement were not recorded, so their individual contribution is unknown. Local reproducer/report: `build/bench_saved_claude.py`, `build/benchmark-saved-claude.json`. All four exits were clean.
