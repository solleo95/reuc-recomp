# Netplay recovery implementation plan

**Owner decision, 2026-10-24:** retain the current synchronous implementation
with its documented deep-replay pauses. Further pause removal/background replay
is deferred. This plan remains a historical progress ledger, not a completed
no-pause acceptance claim. See [current status and resumption notes](../../testing/netplay-known-issues.md).

> Execute inline with superpowers:executing-plans. No commits or pushes requested.

Goal: approved predictive rollback with low input latency, real-time simulation,
stable FPS and matching confirmed state under the existing loss/burst profiles.
Spec: ../specs/2026-10-23-netplay-recovery-design.md

## Constraints

Reuse the current feat/multiplayer checkout and preserve existing uncommitted
work. Tests use isolated saves, muted1280×720 windows, unlocked FPS and distinct
guest cores26/28. Keep compatibility checks and desync diagnostics. Preserve a
baseline executable before rebuilding. Deploy incomplete rollback only on the
owner's explicit request, with the remaining limitations stated.

## Execution gates

### 1. Explicit outer game iteration

Files: game/game_loop.h/.cpp, game/CMakeLists.txt, project/hle_map.txt,
project/uc.cfg, tests/test_game_loop.cpp, tests/CMakeLists.txt.

- [x] Test a reentrant loop trampoline that executes a boundary callback once
  per complete iteration and does not grow the native stack at the back edge.
- [x] Add the8003F670 HLE boundary and8003F674 continuation, preserving the replaced
  addi and all register/stack effects. No network prediction yet.
- [ ] Build and run offline deterministic replay against the preserved baseline;
  require identical guest-visible work and no material frame-time regression.

### 2. Complete checkpoint and exact replay

Files: runtime/sched.h/.cpp, runtime/clock.h/.cpp, runtime/dispatch.h/.cpp,
runtime/guest_mem.h/.cpp; relevant runtime/hle modules, native_input and
game/game_pacing; translator continuation generation if needed.

- [ ] Define resumable parked guest-thread operations at the boundary; capture
  guest contexts and exact continuations without copying native stacks/locks.
- [ ] Add typed snapshot/restore for scheduler/event order, clock, canonical RAM,
  HLE and pacing/input state, with explicit exclusion of non-rewindable output.
- [ ] Test restore/replay across thread handoff, IRQ/timer, DVD, GX/audio and saves.
- [ ] Prove identical full state after replay of actual game iterations. Keep
  prediction disabled until this gate passes; incomplete state is an error.

### 3. Input prediction and recovery transport

Files: game/netplay_game.h/.cpp, common/netplay gameplay transport and tests.

- [ ] Test actual/predicted/confirmed histories, late differing inputs, duplicate
  consistency, bounded storage, and confirmed-state hashes.
- [ ] Add bounded redundant datagrams for gameplay, retaining TCP startup/saves.
- [ ] Integrate checkpoint/restore/replay and canonical timestep decisions.
- [ ] Suppress duplicate effects; commit saves only for confirmed history.
- [ ] Measure and bound replay work so late input does not cause a render stall.

### 4. Real impairment verification

Files: scripts/uctest/ip_loss.py, netplay.py and their tests.

- [ ] Update WinDivert to impair the actual gameplay protocol, including ACKs
  for TCP and datagrams if UDP is used. Assert nonzero captured/dropped traffic.
- [ ] Exercise active input during1%/5% loss and500ms RTT bursts on two harnesses.
- [ ] Compare FPS/frame gaps, simulation ratio, shot-latency distributions,
  correction depth/count and confirmed-state hashes against no-loss baseline.
- [ ] Full build/CTest, save/disconnect/compatibility regression, deterministic
  fight2 paired benchmark and final review before deploying verified binaries.

## Ledger

- Owner accepted rollback visual corrections; implementation requires no further
  confirmation of that product choice or of the already-approved scoped driver.
- Investigation: current KPAD hook is mid-stack.8003F670 is a concrete recurring
  iteration boundary; merely restoring88MiB RAM at KPAD is unsafe.
- Ruling: establish and verify a frame boundary before transport/prediction work;
  avoids a network layer that can request a rollback the runtime cannot perform.
- Boundary verification:176/176 CTest tests passed. Test runs20000 iterations
  without stack growth and checks one original backward-branch poll per iteration.
  The old launcher locale assertion was updated for the owner-requested Singleplayer
  names (English/Russian/Japanese).
- Initial demo comparison rejected mismatched guest work. Cause: editing generated
  hle.cfg was lost by build.bat; splitting the untranslated loop also lost its poll.
  Fixed in persistent hle_map.txt with explicit entry and preserved poll.
- Fixed fight2 comparison:6 runs,2608931 draws in each, identical per-retrace work.
  Baseline/new mean15.830/15.209s; paired delta-3.7%,95% CI[-17.7%,+10.3%].
  First baseline run is an outlier; speed verdict INCONCLUSIVE. Best-vs-best-0.2%;
  peak173.8/174.9FPS. Log:build/netplay-rollback-boundary-bench-fixed.log.
  This proves matching measured work, not a full-state restore or rollback performance.
- Live fight census:build/autotest/1792779209286160100 passed38.6s. Temporary
  instrumentation was removed after capture; build/netplay-rollback-census.txt
  records8 live guest threads at the outer boundary. Main thread returns at8003F828;
  other threads include message waits at803AFCE8/803AC370, VI waits at80343978/
  80343A10, suspension at80262750/80343AF0, and ready threads. A main-thread
  checkpoint alone cannot restore their native continuations. Current Sched stores
  neither resume PCs nor serializable pending HLE operations.
- Remaining architecture work is mandatory before prediction: generated continuation
  dispatch, restartable pending HLE operations, full scheduler/HLE state, external
  callback generation fencing, and suppression of duplicate output/save effects.
  No RAM-only rewind, disabled hashes, or enlarged input delay qualifies as rollback.
- Final boundary build:176/176 CTest passed; temporary census hook removed.
  Real loss1/5% repeated with IP/ACK delay, plus the same application RTT burst.
  All3 failed with buffer exhaustion (1.022s/1.004s/3.903s). Results and report
  identifiers are recorded in docs/testing/netplay.md. Driver/service cleaned up;
  no game process remains. This is explicit failing evidence, not a completed
  rollback implementation. No deployment to the owner's game folder.
- Continuation implementation now unwinds parked Windows stacks cooperatively,
  restores guest contexts, and dispatches generated return-PC entries. Pending
  SDK operations distinguish retry-before-commit from return-after-commit.
  Nested native HLE frames are rejected; rollback mode defers worker preemption
  until an audited resumable boundary. No native-stack byte copying.
- Scheduler tests cover repeated rewind. HLE tests cover queue receive retry,
  alarm cancellation/clock rewind, stale I/O generations and unknown-event
  rejection. Timer callbacks are copied only after explicit EventKind registration;
  captures are audited immutable values plus stable runtime bindings. Mutable
  state behind those bindings is captured separately. Opaque callbacks fail closed.
- Added typed snapshots for OS synchronization/alarms, VI, audio, DVD, GX logical
  resources, MMIO register files, pacing and input. GX recordings are immutable
  shared resources; derived memory caches invalidate after restore.
- SaveFs has an opt-in copy-on-write in-memory filesystem. Checkpoints include
  descriptors/positions; confirmed snapshots publish file replacements atomically
  with existing backup/path protections. Save rollback unit test passes, including
  speculative rename/delete isolation and repeated confirmation.
- Whole-game diagnostic under UC_ROLLBACK_PROBE_RETRACE is restricted to offline
  deterministic input demos. It captures once, advances eight iterations, restores
  and replays. The actual fight2 probe passed eight replayed iterations with equal
  RAM, thread register dumps and virtual clock. A second run captured every frame
  with write-barrier verification and passed (rollback-probe-1792782485259326000).
  This does not yet prove canonical equality of every HLE field.
- Runtime snapshots now include dirty-page RAM sharing, prompt state and audio
  startup waits. Worker contexts reset inherited native depth; continuation records
  preserve the original caller LR when a hook parks before the guest prologue.
- UDP gameplay transport, redundant inputs, prediction and replay are integrated
  experimentally. TCP retains startup/saves and an independent heartbeat; the idle
  control channel otherwise expired after15 seconds, even while UDP was healthy.
  Stream regression verifies liveness while the consumer is idle longer than the
  socket timeout. Saves publish only after confirmed hashes match.
- Two-peer baseline reached gameplay and successfully corrected late inputs, then
  exposed the idle TCP timeout during a long load. Performance figures from that
  run are invalid: the owner was running another demanding game concurrently.
  Owner has now frozen it; repeat performance checks before drawing conclusions.
- WinDivert now covers TCP and UDP, and stress asserts nonzero gameplay datagram
  drops. Stress sends aim/fire/reload from both peers and records simulation rate,
  observed presentation gaps, rollback counters and same-frame confirmed hashes.
  Real impairment reruns, complete HLE equality, replay display suppression and
  final performance validation remain pending. No deployment yet.
- Confirmed-history pruning releases obsolete GX snapshots instead of retaining
  every GPU resource for240 frames. Only speculative snapshots and outstanding
  hash boundaries remain. RAM snapshots share64-page blocks and cache page hashes.
- Replay suppresses presentation while retaining EFB draws, copies, clears and
  serial completions. Local FPS limits now gate presentation independently from
  guest pacing. Dedicated renderer_rollback_smoke passed; the older combined GPU
  fixture has pre-existing validation/descriptor-count failures before new cases.
- Canonical hashes now include guest registers and scheduler ordering, replacing
  per-frame formatted context dumps. Callback captures and every HLE field are
  not yet covered by the canonical hash. Fight2 restore/replay passed8 iterations
  before the block-sharing optimization; repeat after final changes.
- Actual1% loss passed30s with462 dropped UDP packets and30 matching checkpoints
  (netplay-1792785434541013800). Actual5% loss passed30s with2232 dropped UDP packets
  and30 matching checkpoints (netplay-1792785785431912500). The latter uses binary
  UC-NET-11 gameplay frames (108 bytes plus11/event); startup/control remain JSON.
  Both runs retained responsiveness and completed save/disconnect checks. Maximum
  observed frame gaps were58.0/60.7ms: smoothness remains unfinished.
- A later500ms-RTT-burst run failed its preliminary no-loss speed check before
  impairment began: host114.92/client111.92FPS, speed0.958/0.933. This is not a
  successful burst test (netplay-1792785881042739300). Driver cleanup succeeded.
- CPU26/28 profiles repeatedly reproduce late-fight110–113FPS and0.91–0.95 speed
  without network loss. FPS-cap assertions tightened from0.9..1.1 to0.98..1.02;
  stress reports now include one-second simulation windows to expose catch-up.
- A synchronized PMR page pool was tested and rejected: capture time stayed near
 52–56ms/120 snapshots, with no measured speed benefit. Allocation change removed;
  retained-snapshot lifetime/concurrent-read tests preserved.
- Three paired fight2 runs: identical2608931 draws; old/new mean15.618/16.417s,
  +5.3% wall time,95% CI[-7.0%,+17.5%], statistically inconclusive. Guest cycles
  consistently increased (old61–63G, new67–68G); peak175.4→159.8FPS.
  Log:build/rollback-current-bench.log. Existing PGO profile predates continuation
  generation (2026-10-16); refreshing it through the harness before further tests.

### Rendering independence follow-up

Owner explicitly requested separating simulation/rendering and continuing the
existing rollback implementation. Keep CPU26/28, isolated saves, windowed1280x720,
real1%/5% UDP loss and100→500ms RTT bursts. Do not lower a shared simulation tick
in response to a slow peer, disable hashes, or hide slowdown in long averages.

1. Test logical GX completion at submission with physical packets still outstanding,
   duplicate late completion, checkpoint/restore and deterministic event ordering.
   Apply only after the ordering tests pass; retain physical fences for resource lifetime.
2. Resolve render-queue backpressure with complete immutable rendering state. Preserve
   EFB clears and copy dependencies, including cross-frame resources and rollback.
   Audit the original update/draw phases before attempting to skip guest instructions.
3. Test a deliberately stalled renderer, not just a30FPS presentation cap; simulation
   and confirmed state must continue independently with bounded memory.
4. Repeat actual rollback probe, paired deterministic bench, two-peer baseline,
   real1%/5% loss and jitter/bursts. Record one-second speed windows, frame gaps,
   input response and confirmed hashes. Complete review before deployment.

Refreshed PGO was rejected after worse live results; original profile restored.
The original-profile rebuild passed182/182 tests. Dirty-identical RAM pages reuse
their snapshot; the eight-step fight2 restore/replay probe passed afterwards.
Neither result proves rendering independence or improved network performance.

2026-10-24: detached logical GX completion and the newest-complete-frame mailbox
are implemented. The recursive scene-history prototype was rejected after GPU
command amplification and memory exhaustion. Current capture preserves one frame's
original command order, with render-side temporal texture history. No-renderer
completion/checkpoint tests and physical copy/clear/feedback smoke pass. Two peers
with approximately120/30FPS retain1x simulation in both directions.

Snapshots now skip immutable actual-input frames except hash boundaries. Mesh
cache restore preserves validated geometry and invalidates all source-page stamps;
regression tests cover raw restored RAM and changed vertex layouts.

Latest scoped real-loss run is build/rollback-stress-1792792551583833500.log:
1% passes;5% retains120FPS/1x averages but one1s client window is0.945x;500ms RTT
bursts retain confirmed hashes but still produce184–190ms presentation gaps.
All profiles complete30s with30 matching checkpoints each. Harness now rejects
bad1s windows and native present intervals even when aggregate speed is1x.
The rendering-independence implementation is validated in these cases; large
rollback correction smoothness, complete HLE-state hashing and final acceptance
remain open. No production deployment or completion claim.

Additional verification:183/183 full-build tests; three paired `fight2` runs are
inconclusive(−0.4% wall time,95% CI[−5.0%,+4.1%], identical draw counts); actual
eight-step restore/replay probe passes. Scoped review's retained-copy issue is
fixed with weak latest entries and a GPU retirement regression. Already selected
predicted frames intentionally finish their original command sequence.

Input capture now continues while replay/catch-up holds the simulation head back.
Outgoing frames remain immutable; overdue sample slots repeat only held state.
Unit regression covers rewind, catch-up, missed samples and duplicate sample time.
Intermediate profiled burst run reduces correction counts to7/3 but still has
160.9/137.2ms native gaps. Smoothness remains an open acceptance failure.

Final unprofiled run:build/rollback-stress-1792794246173570800.log.1% passes with
worst14.7/13.5ms;5% fails one client57.6ms gap(no rollback during that interval);
500ms bursts fail with121.9/195.7ms gaps and minimum1s speeds0.879/0.794. All
functional checks complete;30/30/29 confirmed hashes match across the profiles.
Post-burst catch-up also fails the first asymmetric-cap speed gate(1.037× client).
Full build183/183 and harness20 tests pass. Driver/process cleanup complete.
Do not report the rollback/performance objective complete. The owner subsequently
requested deployment of that increment; the copy and backup are documented in
`docs/testing/netplay.md`.

### 2026-10-24 replay CPU follow-up

- [x] Separate replay CPU cost from pacing, scheduler and boundary waits.
- [x] Elide unpublished host graphics assembly while retaining guest writes,
  geometry validation, GX matrix/state updates and logical completion ordering.
- [x] Cover live/recorded primitives, normal rendering after replay and the raw
  XF SDK-block side effect; actual-game eight-step RAM/scheduler/clock probe passes.
- [x] Full build183/183; final focused tests5/5; paired offline benchmark shows
  +0.3%,95% CI[-1.0%,+1.6%], identical work. Harness latency telemetry tests22/22.
- [ ] Strict burst smoothness acceptance. Profiled steps improve about9–10%, but
  replay remains CPU-bound and long corrections still pause presentation.

Input-consumption latency is now sampled during stress, not only before it. The
measurement includes harness IPC and acknowledges speculative local consumption;
it does not certify remote confirmation or photon latency. No reduction of the
input buffer or change of simulation tick rate was used for this increment.

Unprofiled final1%/5% profiles pass with30 matching stress hashes each. A burst
run was contaminated by the owner's external hard frame limiter; the owner
confirmed disabling it and freezing the other demanding game. The repeat
`netplay-1792797667081753400` retains119.97/120.00FPS and1x average speed, but
fails128.7/112.5ms frame gaps and0.863/0.881 minimum1s speed ratios. Input
consumption median171.2/171.5ms, p95289.3/280.4ms, maximum324.7/294.8ms. All30
checked stress hashes match; functional checks and driver cleanup complete.
Strict burst smoothness and low worst-case input latency remain unfinished.

### Input queue recovery and remaining vertex work

Scope: `game/netplay_game.*`, its frame tests; `renderer/gx_vertices.*`, replay
callers in `runtime/hle/gx.cpp`, and vertex tests. Preserve all guest-visible
memory/state updates and validation. Test against the same real-loss profiles.

- [x] Validate vertex addresses during replay without converting unused values
  or building vertex/index arrays. Focused tests pass; scoped review found no issue.
- [x] Reproduce permanent input lead: `netplay-1792841671592992100` configured9
  frames, but logs show13 before stress and14 after a burst (peaking21).
- [x] Drain immutable future input slots during forward recovery, while retaining
  wall-time input capture during replay. Test delayed edges and restored nominal lead.
- [x] Use the upper-quartile startup RTT plus the existing two-frame margin;
  verify isolated probe outliers cannot permanently enlarge input latency.
- [x] Full build, actual-game restore probe, serial paired bench and all loss
  profiles. Report worst intervals and latency, not only average FPS/speed.

The queue regression now covers retained press/release edges, replay-time capture
and return to the configured lead. Startup uses the upper quartile of nine RTT
samples for both buffer sizing and host start alignment (UC-NET-12). A QTE window
marker alone no longer causes correction when the input contains no shake action;
tests compare the actual QTE filter results.

A profiled spike run (`netplay-1792842198399300200`) exhausted rollback history.
Its old generic error does not establish whether input, acknowledgement or hash
progress stalled. Add diagnostic counters and fix independently established
transport weaknesses: initially send skipped sample slots, repair up to eight
old inputs together, and deliver each confirmed hash through TCP as well as UDP.
Keep the 240-frame history and all acceptance thresholds unchanged.

Final unprofiled real-loss runs: `1792842887176045600` (1%),
`1792842983033460200` (5%), `1792843078170312300` (spikes). All functional checks
complete and all 30 stress hashes per run match. Loss-only profiles pass strict
performance gates. Spikes still fail: host/client worst gaps 154.4/167.7 ms,
minimum one-second speeds 0.804/0.808. Input command completion medians improve
to 107.6/107.7 ms, but maxima remain 304.4/293.4 ms. This metric includes the
requested hold and release, not initial press-to-photon. Driver cleanup passes.
The actual eight-step RAM/thread/clock restore probe also passes.

Remaining acceptance failure: CPU replay still executes guest task callbacks and
their state writes. Removing unused host vertex conversion does not eliminate
that work. Do not skip arbitrary guest graphics callbacks or report the overall
no-pause requirement complete.

Final verification: CTest 183/183 and harness tests 22/22 pass. Serial three-pair
offline bench: 15.811 s baseline vs 15.932 s current, +0.8%, 95% CI [-0.1%, +1.6%],
inconclusive, identical draw counts. An earlier isolated +43% slowdown did not
recur, but its cause remains unconfirmed; retain both logs. Strict spike
smoothness and low worst-case input latency remain open acceptance requirements.

### Replay-only CPU investigation

- [x] Capture stacks only while restoring/replaying, resolve them with the exact
  executable's linker map, and identify the dominant avoidable work.
- [ ] Add a regression for the identified cause, implement the smallest safe
  correction, and preserve all guest-visible state and input timing.
- [ ] Run the actual-game restore probe, paired deterministic benchmark and
  the same two-harness real-loss/spike profiles. Keep strict smoothness gates.

Diagnostic scope: `game/cpu_sampler.*`, the replay boundaries in
`game/netplay_game.cpp`, and an opt-in environment switch. A sampled run establishes
where CPU goes; only unprofiled runs count as performance acceptance.

Replay-only run `netplay-1792844400627709400` produced 651 samples. No single
dominant avoidable wait was identified. A transpose kernel at 80137F78 is a possible
small local optimization, but does not establish a route to the 50 ms gap budget.
Cross-world replay currently fails ownership constraints in RAM/scheduler/HLE.
The proposed architectural route and required safety checks are recorded in
`docs/superpowers/specs/2026-10-24-background-replay-design.md` for owner review.
