# S15: the guest and the GPU did not overlap (2026-10-20)

Owner's report: DLSS lowers the FPS when no effect is on, and the GPU stays far from fully used (RTX 4080: 60% at 218 FPS without ray tracing, 75% at 120 FPS with it).

## Evidence (frame log `UC_FRAME_LOG`, live harness runs at 2560x1440, the owner's own `debug/frames-*.csv` shows the same numbers)

* Without effects the game is bound by the guest thread: ~4.6 ms per frame (`t1_ms`), GPU 0.6 ms per submission. An upscaler adds GPU time and render-thread CPU, so it cannot raise the FPS there, but it must not lower it either (209 -> 186/194 FPS was measured).
* With path tracing the frame was **guest time + GPU time**, not the longer of the two: interval 7.8 ms = 4.8 ms + 2.9 ms (high), 17.4 ms = 4.8 ms + 12.3 ms (ultra). `idle_ms` (every guest thread blocked) grew by exactly the GPU time while `seal_wait` stayed 0.
* The idle report at the end of the frame log (`idle_by_`) puts it in one place: the guest thread sleeps in `GXWaitDrawDone` (`8003A6D0`, the call after `GXSetDrawDone` at `8003A674`), once per frame, 3 ms per wait.

## Causes (two)

1. `GXWaitDrawDone` ended when the GPU completed the draw-done packet, i.e. when everything queued before it had run, including the heavy effect passes (F3/F4 had measured this wait at ~1 ms; it is the whole GPU time of the frame now). Signalling it **before** the packet is even submitted breaks boot (F3, F4), but signalling it **when the render thread has submitted the packet** does not: the packet owns everything the GPU reads. The intro movies reach the title in the same 139 s with and without it. `Packets` still bounds the frames in flight by the real completion, so the input latency does not grow. Deterministic mode keeps the real completion. Escape hatch: `UC_FINISH_AT_GPU=1`.
2. `kFrameSlots` was 6 GPU submissions in flight, a game frame is ~23 packets: the render thread could not take the draw-done packet until the GPU had finished the packet six back (3 ms behind the effect packet). 16 slots end that stall (`request -> take` 3.0 ms to 1.2 ms, render retries 532 to 35 per second). The per-frame ray tracing state is decoupled from the slot count (`kRtFrames` = 5) and the passes' descriptor-set arrays hold 64 slots.

Not the cause: `kMaxFramesInFlight` (3 instead of 2 changed nothing), the number of slots beyond 16 (48 gains ~5% for hundreds of MB of host-visible buffers).

## Result (fight scene of the first car, same scenario, 2560x1440, one run each)

| Setting | interval before | after |
|---|---|---|
| no effect, no upscaler | 4.8 ms (209 FPS) | 4.6 ms (215) |
| no effect, DLSS Quality / Performance | 5.4 / 5.1 ms (186 / 194) | 4.6 / 4.6 ms (216 / 218) |
| ray tracing high, native | 7.8 ms (128) | 6.1 ms (163) |
| ray tracing high, DLSS Performance | not measured | 5.2 ms (192) |
| ray tracing ultra, native | 17.4 ms (57) | 15.4 ms (65): the GPU is the limit now (12.8 ms effect packet + ~2.6 ms of the rest) |
| ray tracing ultra, DLSS Performance | not measured | 7.0 ms (143) |

With the effects off the game is still guest-bound (the GPU is mostly idle by construction); with them on the frame time is the longer of guest and GPU.

## Tools

`UC_FRAME_LOG` columns `interval_ms busy_ms idle_ms t1_ms gpu_ms`; the idle report (`# idle ... t1:<waitkey>@<lr>`) at the end of the log names what a blocked guest thread waits on; `[profile] loops/retries` (with `--profile`) counts render() calls refused for a busy slot.
