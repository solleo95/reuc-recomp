# GX packet building on a worker thread: estimate (2026-10-15, branch gx-worker-thread, not built)

Question: how much faster would the guest step get if the GX HLE work moved to a worker thread?

Guest main thread at the ~140 Hz ceiling (7.1 ms/frame), classified from sampler stacks:

| Category | Share | Time |
|---|---|---|
| Translated game code | 44.9% | ~3.2 ms |
| Waiting for the GPU in `GXWaitDrawDone` | 14.2% | ~1.0 ms |
| GX HLE (mesh 9.1, immediate draws 5.8, display lists 4.5, other 9.7) | 29.1% | ~2.1 ms |
| Other waits, mmio | ~8% | ~0.6 ms |

Upper bound: with `UC_SKIP_MESH=1` (temporary experiment, removed) every mesh draw returns at once: step 135 -> ~170 Hz. This overstates the gain because the renderer also loses all mesh work.

Realistic: most of the HLE cost is capture that must run on the guest thread at call time (reading matrices, arrays and textures from guest memory, mutating the native state later draws depend on). What can move is the SDK-shadow parsing and `DrawPacket` assembly: ~5-6% of the guest thread (~0.4 ms, +3-10% step) for a refactor of ~40 HLE functions plus error propagation from the worker and a `DrawDone` handshake. Not worth it now.

Other findings from the same session:
- Early `GXDrawDone` completion (flag set at once, callback delayed by 0.5 ms) breaks the game again: boot never reaches DVD reads. The game relies on the real completion timing.
- Window focus: process priority, present mode (fifo/mailbox/immediate) and EcoQoS/timer-resolution opt-out change nothing in the unfocused harness window (guest-bound at ~135 Hz). If a focused window is faster for the owner, suspect GPU contention with OBS/Parsec or the NVIDIA background frame-rate limit; needs an A/B on the owner's machine. `uc.exe` now opts out of Windows power throttling (EcoQoS, timer-resolution override) anyway.

Better levers for the guest step: the translated game code (45%): compiler flags/LTO/PGO and register caching in the generated code; and the render-thread vertex upload (87k vertices/frame) which shortens the `GXWaitDrawDone` tail.
