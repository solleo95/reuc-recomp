# Background replay proposal

Status: deferred by the owner on 2026-10-24 after resource clarification.
Retain the current synchronous rollback. Background replay is not implemented or
performance-validated; do not resume it automatically. Current issues and evidence:
[netplay-known-issues.md](../../testing/netplay-known-issues.md).

## Required behavior

Keep the active simulation and presentation advancing at their normal cadence
while a late remote input is corrected. Preserve the existing low input delay,
immutable input history, save isolation, and confirmed-state validation. Brief
visual corrections are already accepted by the owner. Do not weaken the existing
performance gates or report repeated frozen frames as useful rendering.

## Evidence and alternatives

The final unprofiled synchronous implementation has 154/168 ms present gaps under
the existing RTT spike profile. Replay takes roughly 6–7 ms per step; corrections
reach about 21 steps. A replay-only stack sample collected 651 samples in
`build/rollback-replay-samples-1792844400623634300.txt`, resolved with that build's
`build/game/uc.map` into `build/rollback-replay-hotspots.txt`. Work is spread across
guest code, native math, GX handling, and checkpoint/transport work. This sample
does not prove that further local optimization is impossible.

Increasing the input buffer would trade pauses for persistent input latency.
Presenting extrapolated graphics alone would leave simulation and input
consumption stalled. Local kernel optimizations may help throughput, but do not
remove the synchronous dependency. The proposed architectural change is an
isolated replay world on another CPU thread.

## Proposed ownership

Maintain two runtime worlds in one process. Each owns its RAM, scheduler, virtual
clock, MMIO state, HLE state, pending I/O, and input pads. Immutable game assets
may be shared. Only the active world publishes graphics/audio and commits saves;
the worker performs simulation/replay without GPU submission or audible output.
This is a proposal to use additional CPU and RAM, not a measured resource budget.
It does not add a game window or a second renderer.

### Resource measurements and provisional budget

The owner requested resource clarification before approving implementation.
The current synchronous build was measured in two harness with the same real
loss/spike profile, without CPU stack sampling. Resource counters were sampled
every 250 ms. Run: `netplay-1792846091477680500`; raw counters:
`build/rollback-resources-1792846091407006300.jsonl`. There were no measurement
errors or leftover monitors; WinDivert cleanup completed. All 30 stress hashes
matched and functional checks completed. This is resource evidence for the
existing implementation, not validation of background replay.

| Current process | Normal RSS median | Stress RSS median / max | Stress private commit max | CPU normal / stress mean |
| --- | ---: | ---: | ---: | ---: |
| Host | 1758 MiB | 1803 / 1854 MiB | 3403 MiB | 1.24 / 1.07 |
| Client | 1995 MiB | 2020 / 2076 MiB | 3490 MiB | 1.23 / 1.03 |

CPU values are CPU seconds per wall second across all process threads (1.0 is
one logical CPU continuously occupied); they are not percentages of the whole
machine. Normal samples cover the last nine seconds before stress. RSS means
resident physical memory; private commit is a separate, overlapping measure
and must not be added to RSS. The test machine has 16 physical cores / 32 logical
CPUs and approximately 64 GiB installed RAM.

For the proposed extra world, the known mutable guest RAM is 88 MiB plus 64 KiB
of locked-cache mapping. HLE state, thread contexts, changed snapshot pages and
retained resources add an unmeasured amount. Immutable assets and snapshots may
be shared; duplicating the entire measured process is not the design.

Provisional planning allowance: reserve one additional physical CPU core and
up to 1 GiB additional RAM per game instance for the prototype. This is a budget
to validate, not a measured prediction or guaranteed ceiling. For two local
harness, reserve two extra physical cores and 2 GiB additional RAM. There is one
renderer and worker GPU/audio publication is prohibited; GPU/VRAM duplication
is not intended, but retained resource overhead still needs measurement.

Catch-up consumes more work than the initial backlog because the active world
keeps advancing. At a fixed 120 steps/s and a worker cost of 6–7 ms/step, clearing
21 steps takes approximately `21*c / (1 - 120*c)` = 0.45–0.92 seconds. This is a
conditional model assuming the worker keeps that throughput, receives usable
inputs and does not restart. It describes correction completion, not added local
input delay. If worker cost reaches 8.33 ms/step, it cannot catch the moving head;
additional isolation alone then cannot satisfy the design.

After this resource clarification, the owner chose to retain the current version
and defer this proposal for a possible later task.

Current blockers are concrete:

- `game/game_checkpoint.cpp` binds snapshots to a RAM pointer and input objects.
- `runtime/sched.cpp` binds snapshots to a scheduler, Thread pointers and event
  closures. Cloning must remap stable thread IDs and use typed/rebindable events.
- `runtime/clock.cpp` and `runtime/mmio.cpp` contain process-wide mutable state.
- `runtime/hle/gx.cpp`, `runtime/ax_audio.cpp`, `runtime/hle/dvd.cpp` and other HLE
  modules bind global state and callbacks to one scheduler/memory region.
- `game/uc_main.cpp` initializes one GuestRunner, scheduler and packet queue.

Do not remove cross-owner restore checks. Introduce an explicit validated clone
operation; do not shallow-copy callbacks or run two restores on shared globals.
Main-thread initialization, guest threads, and I/O workers must bind the intended
world explicitly. Async completion must carry world identity and generation.

## Correction and handoff

One coordinator owns network input history and starts at most one replay job.
The active world continues predicted forward simulation. The worker starts from
a compatible checkpoint preceding the earliest changed input, consumes immutable
history and catches the moving active frame. New corrections invalidate stale
work by generation; they must not create additional workers.

Promote a completed worker only at a shared simulation boundary after validating
its generation, input prefix and frame number. Publish a complete frame from the
promoted state. Retire the old world's publication generation so queued stale
frames/audio/I/O cannot leak through. Commit saves only from confirmed history.
The next correction must start from a checkpoint belonging to the corrected
history; the old speculative world's snapshots cannot be assumed compatible.

Worker throughput must exceed forward simulation throughput to converge. This
must be measured; moving replay to another thread alone does not establish it.
If the worker falls behind, preserve bounded memory and explicit session failure
handling. No claim of arbitrary-outage tolerance follows from this design.

## Acceptance before enabling

1. Clone a checkpoint between isolated worlds and prove RAM, thread continuations,
   virtual time and relevant HLE/event state agree after identical inputs.
2. Run both worlds concurrently with different inputs; verify isolation, stale
   I/O rejection, single save commit, and no worker GPU/audio publication.
3. Test handoff during input edges, QTE changes, resource creation, disconnect,
   and a newly arriving older correction. Retain snapshots safely across handoff.
4. Run the actual-game restore probe and paired deterministic offline benchmark.
5. Run two network harness on distinct physical cores with the previous real
   1%/5% losses, RTT 100 ms, ±10 ms one-way jitter, and periodic extra 200 ms
   one-way delay. Reserve separate physical cores for replay workers.
6. Preserve strict gates: native present maximum 50 ms and p99 16.7 ms; one-second
   simulation ratios 0.95–1.05; average ratios 0.98–1.02; no mismatching confirmed
   state, crash or pending input. Compare input latency with the current build.
7. Repeat asymmetric render caps and measure peak RAM plus worker convergence time.

This is a cross-cutting runtime change. No implementation or claim of satisfying
the no-pause requirement should precede the isolation and handoff checks.
