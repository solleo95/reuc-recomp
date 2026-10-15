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
