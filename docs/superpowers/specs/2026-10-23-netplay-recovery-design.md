# Netplay recovery: implementation design draft

Status: owner selected rollback and accepted brief visual corrections on2026-10-23;
implementation authorized. Runtime restore/replay gate remains outstanding.
This document describes work to implement, not a working rollback implementation.

## Required behavior

Two locally simulated/rendered games must survive the existing unstable-network
profiles: actual 1% and 5% packet loss, RTT100ms with ±10ms jitter per direction,
and one-second RTT100→500ms bursts. Keep responsive input, normal simulation
speed, and matching confirmed state. Preserve launcher readiness and temporary
client saves. Do not remove desync detection or replace missing input silently.

The testable scope is these bounded profiles. No finite buffer can promise
uninterrupted synchronized play through an arbitrarily long disconnection.

## Evidence and approaches

Current UC-NET-9 applies delayed inputs at a negotiated fixed offset. At14 frames,
real loss plus IP/ACK delay exhausted input after0.829s/1.294s. The RTT burst also
exhausted the buffer. Exact reports are recorded in docs/testing/netplay.md.

1. More buffering or waiting can protect determinism but raises input latency or
   stops simulation. This does not satisfy the requested behavior.
2. Redundant datagrams recover isolated losses without TCP head-of-line blocking.
   They cannot deliver a genuinely late input before it exists at the receiver;
   this alone does not solve the delay-burst case.
3. Prediction with deterministic rollback addresses both loss and late input.
   It requires complete restore/replay support and may correct speculative
   visuals after a late input arrives. The owner selected this approach.

## Runtime work required before network prediction

The current native call stacks are part of guest execution:

- runtime/dispatch.cpp and runtime/guest_call.cpp enter translated native calls.
- runtime/sched.h and sched.cpp retain native OS threads, wait queues, timed
  closures, IRQ context and scheduling order.
- runtime/ppc_rt.h and optimized translated leaf routines can keep live registers
  in C++ locals across polling. Ctx is not a complete continuation.
- runtime/guest_mem.cpp maps MEM1, MEM2 and locked-cache aliases; snapshot canonical
  storage once and restore aliases consistently.
- runtime/hle/os_sync.cpp and os_misc.cpp own synchronization/alarm globals.
- runtime/hle/dvd.cpp, GX, audio and save I/O own external effects.
- game/game_pacing.cpp and native input own additional mutable state.

Restoring RAM at NetGame::poll is unsafe. Establish a resumable execution boundary
first: generated code must materialize every live guest register and an exact
continuation; yielding HLE calls must have explicit resumable state. Scheduler
snapshots must use stable IDs and replayable event data rather than copying
condition variables, native stacks, arbitrary closures or handles.

A restore operation must cover RAM, CPU/continuations, scheduler, virtual time,
HLE and pacing/input state as one consistent checkpoint. Pending external I/O
must be drained or represented by deterministic results in the checkpoint.
Replay must suppress duplicate audio, GPU submissions, file writes and other
irreversible effects. Save writes become committed effects only after their
simulation frame is confirmed by both peers.

First acceptance gate: checkpoint, advance with recorded inputs, restore, then
replay those inputs. Full state hashes and event order must match. Exercise
thread handoff, sleep/IRQ, DVD completion, frame submission and save operations;
RAM equality alone is insufficient. No deployment before this gate passes.

## Network integration after restore/replay exists

Keep reliable TCP for lobby, compatibility and save transfer. Use bounded gameplay
datagrams with session identity, frame numbers and redundant recent input, plus
acknowledgement of received/confirmed frames. Validate endpoint, bounds, duplicate
consistency and session membership before accepting gameplay data.

Maintain separate actual, predicted and confirmed input histories. Predict remote
continuous state conservatively; never invent new fire/reload/knife/QTE edges.
Local input remains immediate. A late actual input that differs from prediction
restores the preceding checkpoint and replays to the present. Confirmation only
advances over frames for which both actual inputs are known. Compare state hashes
at the same confirmed frame, after corrections, retaining mismatch diagnostics.

Current pacing includes exchanged wall-work estimates in the simulated timestep.
Record the canonical timestep decisions alongside inputs; replay must reuse them.
Never recompute virtual time from the cost of replay or independently measured
local frame time. QTE epochs and consumed-input acknowledgements must also rewind
or advance with their corresponding confirmed timeline.

Use bounded history and measure checkpoint/replay cost before choosing final
window size. The window must cover the500ms RTT profile with scheduling margin.
Avoid a full88MiB copy every input poll; evaluate dirty-page/delta storage at the
verified execution boundary. Rendering and input collection must not block on
network I/O or an unbounded replay loop. Whether replay fits the frame budget is
an acceptance result to measure, not an assumption.

## Verification and release gates

1. Restore/replay determinism and effect suppression tests pass.
2. Protocol tests cover loss, duplication, reordering, late differing input,
   stale sessions, malformed datagrams and bounded shutdown.
3. Harness impairment covers the actual gameplay transport, including UDP if
   introduced. A TCP-only WinDivert filter must never produce a UDP-network pass.
4. Run identical two-harness profiles on CPU26/28 with isolated saves, muted
   1280×720 windows and fps-limit0. Include active P1/P2 aim, fire, reload and QTE
   events during impairment, not only an idle soak.
5. Measure confirmed-state agreement, rollback count/depth, input-to-shot latency,
   both peers' simulation ratios, FPS and frame-gap percentiles throughout stress.
   Compare against the same build without impairment. Preserve failure artifacts.
6. Run deterministic fight2 paired benchmarks against a preserved executable;
   repeat3 pairs where needed, then confirm live speed1 performance. Reject a
   change that merely trades network exits for slow motion or long replay stalls.
7. Full build/tests, existing launcher smoke, save isolation, incompatibility and
   disconnect tests pass before deploying the verified build. No commits/pushes.

## Accepted interpretation and implementation decisions

The owner accepts brief speculative visual corrections. Confirmed state must
match, and FPS/game speed remain acceptance gates. Network mods remain disabled.
Target local input-to-shot response below100ms at nominal RTT100ms, with no
increase during the bounded impairment profiles. Measure enough shots for a
distribution rather than reporting a single favorable sample.

KPAD polls are not themselves complete simulation steps. The recurring main loop
at8003F670 (inside8003F600) is the first candidate for an explicit whole-iteration
execution boundary. Its standard translated body keeps guest registers in Ctx.
The boundary must be verified through menu, gameplay and loading transitions;
other guest threads and HLE state still require consistent restoration.
