# Native 240 Hz game step: feasibility probe (2026-10-15)

Question: can the guest run its own frame at 1/240 s with correct game speed?

Probe (throwaway, `F2-native-240-probe.patch`, env `UC_SPIKE_HZ=240`): `hle_GameFrameWait` waits on a wall-clock deadline (scheduler sleep + 1.5 ms spin) instead of a VI retrace, feeds the real elapsed time as dt (`object+4/+8`), counter `+0x20=1`. VI/audio stay 59.94 Hz. A sampler thread records RIP of the guest main thread every 0.5 ms; `scripts/spike_resolve_samples.py` resolves it through a linker `/MAP`.

## Results (RTX 4080, 2x render scale, 1280x720 window, train chapter)

| Phase | Guest frame cost | Presents/s | Game time per wall second |
|---|---|---|---|
| Boot, title, menus | ~5.0 ms | ~200 | 1.00 s |
| Train gameplay | ~20.5 ms (6.5 ms blocked in `Packets::seal_batch` waiting for the renderer) | ~49 | 1.00-1.02 s |

- The mechanism works: dt from real time keeps game speed correct, menus look right at ~200 FPS.
- Gameplay frame budget at 240 Hz is 4.17 ms. Measured non-blocked cost is ~14 ms.
- Main-thread samples (renderer-blocked time excluded): ~45% translated game code (flat, no hot spot; ~6.4 ms/frame), ~41% `runtime/hle/gx.cpp` (GX recording; ~18% in `variant<RecordedDraw,...>` construction/copy, 3% `DrawPacket` move, 2.4% `vector::push_back`, 4% `gx_fifo_write`), ~4% `gx_vertices`, ~2.4% MMIO.

## Conclusion

Guest game logic alone costs ~6.4 ms per frame, above the 4.17 ms budget even with free rendering. Native 240 Hz stepping is out of reach with the current recompiled code. Removing the GX recording copies (largest single item) and moving packet building off the guest thread would bring the frame to roughly 8-10 ms: enough for a stable 60-100 Hz step, not 240.

Next options: stable 60 Hz step (GX recording cost first), then presentation at 240 Hz with interpolation between guest frames.
