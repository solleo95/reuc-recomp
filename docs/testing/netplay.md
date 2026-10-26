# Cooperative play

## Current mode: input packets on a shared tick (2026-10-26)

Only input and state hashes cross the network. Both PCs run the whole game on the
deterministic virtual clock at one shared simulation tick: 60 Hz by default, then
chosen automatically from 30 (the original rate), 60, 120, 144 and 240 Hz
(`--net-tick` sets the starting tick, agreed in the startup message). A frame limit
(`--fps-limit`) never changes the tick: network play steps at the tick and the limit
only caps presents. Each PC renders and presents at its own rate (detached rendering:
the renderer shows the newest complete frame), so a slow GPU lowers only its own
picture. The simulation itself must run at the tick on both PCs; a PC that cannot is
handled automatically:

* **Lead limit.** At most `net_prediction_limit` (6) steps run on predicted remote input;
  the next waits for the other PC's input, the virtual clock gives the waited time back
  (the game slows down, it never races to catch up) and rollback replays stay short.
  Nothing ends the session for a slow peer any more (it used to exit with "input
  sampling exceeded rollback history"); 10 s without input is reported as an error.
* **Automatic tick.** Each PC measures the wall time of one simulated step (waits
  excluded) and reports it in its acknowledgements. The host's `NetTickPolicy` drops
  the tick one rung when the slower PC needs more than 85% of the current frame for
  four seconds, and climbs one rung after twenty seconds under 65% of the next rung's
  frame (a drop soon after a climb doubles that wait, up to 160 s). The change is a
  message `{tick, at}` three seconds ahead; both PCs switch at step `at`, every
  replay through that step applies it again, and hashes and history use fixed
  frame counts (60 and 120), so the peers stay hash-equal across a change. The
  input pipeline is sized in frames for the 60 Hz tick at startup and kept.
* **Host display above the tick.** Frame generation (DLSS-G, FSR 3.1) is allowed in
  network play: it adds presented frames only. The displayed rate of the host is the
  tick times the multiplier (120 at 60 Hz with a 2x generator); the harness counters
  count rendered frames only.

`UC_NET_CLIENT_STEP_US=<us>` makes every step of the joining player that much slower
(tests of a weak PC). `[netplay] rollback count ...` lines show tick and step costs.
Regression: `cd scripts`, `python -m uctest.netplay --exe dist/uc.exe --speed 1`
(controls, FPS ceilings, hashes), `python -m uctest.netplay_slow_peer` (with
`UC_NET_CLIENT_STEP_US=12000` it must settle at 30 Hz, speed 1.0, equal hashes).

Both players open Multiplayer. Create starts a listener on all local IPv4 addresses,
including localhost, with automatic free-port selection. Copy the displayed LAN or
Internet `IP:Port` into the client's single address field. The game starts automatically
after connection; no Ready checkboxes. LAN does not need router NAT traversal.

## Archived: host-authoritative video build (2026-10-26, replaced, see `build/backup-host-auth`)

That build streamed the host's picture to a thin client. Its stream used H.264 (Media Foundation, High profile or Main where the encoder
requires it, CBR, no B-frames, keyframe every 120 frames) up to 1280x720 (multiples of
16) at up to 60Hz and stereo PCM32kHz. The encoder is the first hardware H.264 MFT
that works (NVIDIA first, then AMD, Intel, ...) and falls back by itself to the Windows
software encoder when none exists or one stalls or fails (`UC_STREAM_ENCODER=software`
forces it). The client needs the Windows H.264 decoder (not in Windows N editions
without the Media Feature Pack); decoded NV12 goes to the GPU as an NV12 texture.
Encode/decode and network I/O use worker threads; GPU readback goes through a bounded
nonblocking ring in cached host memory (write-combined memory reads at a few hundred
MB/s and cost the host 15 ms per frame); streaming at 60Hz costs the host about 4% FPS.

Rate control (`VideoRate`) is automatic. The bitrate starts at 16 Mbit/s and rises 15%
every second of calm up to 30 Mbit/s; it falls by a quarter when captures are replaced
because the in-flight window is full (more than 20% in half a second) or when the
receipt delay is 60 ms above its best (a standing queue). The floor is 2.5 Mbit/s;
`UC_STREAM_KBPS=<kbit/s>` pins it. The window of frames sent but not yet decoded
follows the path's best round trip (2 frames on a LAN, up to 6): every encoded frame
must reach the decoder, so newer captures replace the one waiting while the window
is full and the host never waits. The client shows each decoded picture at once (no
jitter buffer, at most two queued, newest wins). Audio queues retain at most100ms.
A hardware-encoded stream decodes with no extra delay; the software encoder's stream
leaves one picture in the decoder (17 ms at 60Hz). Frame rate depends on host/network/
client capacity. Insert releases the client's cursor; graphic settings belong to the host.
Link tests: `DelayRelay.set_bandwidth` (scripts/uctest/netem.py) throttles the TCP relay.

The host retains the ordinary GX completion queue so guest loading/audio threads can
run while graphics work is pending. Video delivery runs independently. Capture uses
phase-based deadlines; the client buffers at most three images and follows the
measured source rate, up to60Hz. A single image is released after50ms even when the
source delivers fewer than4FPS. HDR native prompts are composited into the SDR stream
on the encoder worker.

The video build's regression script is gone with it. The rollback sections below describe
the input-packet netcode that is current again (with the tick and lead changes above).

On the affected host, Windows Filtering Platform identified ESET Security blocking
incoming TCP and UDP for `dist-raw/uc_launcher.exe` (filter70661). ESET Advanced setup
→ Network access protection → Firewall → Rules → Edit: allow inbound TCP/UDP for the
actual `uc_launcher.exe` and `uc.exe` paths, remote subnet192.168.50.0/24, ports any
(automatic port fallback). Put the rules above blocking rules. Security configuration
is user-managed; it is not changed by the game.

## Historical rollback implementation and measurements

**Owner decision, 2026-10-24:** retain the current synchronous rollback;
background replay is deferred. See the [current known-issues and handoff record](netplay-known-issues.md)
for the remaining pauses, measured evidence, other limits, and the proposed future
fix. Retaining this version does not mean burst smoothness passed acceptance.

Run `dist/uc_launcher.exe` on both computers. Click **Multiplayer** in the bottom
action row, beside **Singleplayer**. The host clicks **Create**; the other player enters the host's
IPv4 address, the same port (default **27888**, TCP and UDP), and clicks **Connect**.
After both show connected, each checks **Ready**. Both launchers then start their
local game once; the games wait for each other before accepting game input.
**Cancel** closes the lobby and releases the port.

The host controls P1 (red), the client P2 (blue). Navigate the initial menus with
P1. On character selection each player selects a character; wait for both **1P**
and **2P** stamps before proceeding. Confirm P1's equipment, then P2's equipment
and the final start dialog using P2's controls. Both see the original
shared-camera cooperative game and use their own configured native controls.

Use the same `uc.exe` and original game data on both computers. Before any guest
code runs, the startup check compares the executable, main DOL and simulation
settings. The host then sends its save snapshot to the client. The client uses a
fresh `.netplay-session-*` directory beside its normal saves for the whole session;
its personal saves are never opened for writing or replaced. The host keeps its
normal save progress. Backups/deleted-save folders are excluded from the transfer.
Both peers verify the snapshot before starting the game. Transfer limits are
256 files/directories and 16 MiB; unsafe paths, links and truncated transfers fail.
The temporary client directory is removed after normal exit, disconnect or a
handled startup error. A forced process kill or power loss can leave that temporary
directory behind; it is never reused as a personal save.

The current repository build uses experimental prediction and rollback (UC-NET-12).
TCP carries startup, saves, heartbeat and a reliable copy of each newly published
confirmed-state hash. Nonblocking UDP carries redundant input and acknowledgements:
four recent frames plus up to eight oldest unacknowledged frames per iteration.
Skipped sampling slots receive an initial send even outside the recent window.
Nine startup round trips select a
fixed input delay: half the upper-quartile RTT, rounded up to120Hz frames, plus two
frames. This estimate also aligns startup, so isolated retransmissions do not set
permanent input lag or start skew. A selected RTT above433ms is rejected at startup.
Missing input predicts the previous held
buttons and aim, with transient actions cleared. Different late input restores the
earliest affected checkpoint and replays. The history budget is240 input frames.
Changing only the QTE window marker requires no replay when input contains no
shake gesture; held/edge/event shake inputs still compare their window markers.
Confirmed input cannot change; checkpoints are retained for predictions and120-frame
hash boundaries. Saves publish only after both confirmed hashes agree.
Input capture continues on wall time during replay. Forward recovery consumes
already-sent future slots before sampling more, restoring the negotiated buffer
instead of retaining the extra queue length after a correction.

Snapshots restore guest RAM, thread continuations, scheduler events, HLE state,
input, pacing and the speculative save filesystem. Confirmed hashes cover RAM and
canonical scheduler/register state; they do not yet cover every HLE field or opaque
callback payload. The startup barrier aligns both simulation epochs.

Simulation uses its own120Hz pacing. Detached graphics packets complete logically
without waiting for GPU fences. The renderer takes the newest complete immutable
frame, so a30FPS presentation limit does not force simulation down to30Hz. Physical
GPU fences still protect resource lifetime. Original copy/draw/clear order within
the selected frame is preserved. Temporal GPU effects use the last physically
rendered texture at the same guest address; an unseen history starts blank. These
pixels are presentation state and can visibly correct after rollback. Simulation
still executes the original guest rendering callbacks, so expensive CPU replay can
cause visible pauses; constant presentation during large corrections is unproven.
Simulation settings are fixed for the session; local graphics and bindings remain
local. Mods, opening movies, tutorials and frame generation are disabled. Tab
fast-forward and the port's P pause are disabled; Escape uses the original game's
synchronized pause. A disconnect, timeout or desync ends the game with an error.
Delayed native shake gestures carry a per-player QTE-window epoch; changing the
actor, mode, target or open window resets partial motion and discards stale shakes.

The host must be reachable at the selected TCP and UDP port. LAN uses the host's LAN IP.
**Create host** now attempts UPnP port forwarding on the home router; see
[router mapping](netplay-router-mapping.md) for status, cleanup and limitations.
Routers without enabled UPnP still require manual forwarding or a virtual LAN.
There is no CGNAT relay, matchmaking, mid-mission join or reconnect.
Each computer must have enough CPU capacity for simulation and replay.
The negotiated buffer trades input response time for fewer corrections; outages
exceeding retained history cannot continue. The latest `dist` rollback build has
not been deployed to the owner's game folder; an earlier rollback increment was
deployed as recorded below. Historical results below refer to
their named builds and do not constitute acceptance of the current one.

## Automated verification

Double-click `test-multiplayer.bat` in the project root to run the development
scenario: two harnesses, RTT200ms, ±20ms jitter per direction, different personal
saves and a 60-second soak. It uses the current `dist/uc.exe` and Python3.13 via
the Windows `py` launcher, then keeps the console open with the result.
The host uses CPU 26 and the client CPU 28. Windows topology confirmed they belong
to distinct physical cores on this test machine (CPU 27 is CPU 26's SMT sibling).
The owner authorized this network-only change; offline tests and benchmarks still
use CPU 26. These pinned indices must be reselected when testing on another machine.

From the project root:

```powershell
.\build.bat --no-install
ctest --test-dir build --output-on-failure
```

From `scripts`:

```powershell
python -m uctest.netplay
```

Optional arguments: `--exe`, `--port` (uses that port and the next for a mismatch
check), `--speed`, `--connect-delay 18` (slower client startup). The scenario starts two real `Game` harnesses on loopback,
with separate copied saves/config, muted 1280x720 windows and `--fps-limit 0`.
It selects both characters/loadouts, checks independent aim/fire/reload/knife,
changes only the client's FPS limit without changing simulation, compares memory
hashes, checks bounded exit after the client quits, and starts
another pair with incompatible simulation settings to verify startup rejection.
JSON report, failure artifacts and logs are in `build/autotest/netplay-<timestamp>/`.
Successful runs avoid synchronous screenshots: GPU readback and PNG encoding
stall the render thread and distort the live frame-time graph.
Read-only `peek` commands reuse the existing harness protocol for P2 assertions.

`python -m uctest.netplay --speed 1 --latency-rtt 200 --different-saves --soak-seconds 60`
starts a fresh pair with delay active from connection/save transfer onward. Repeat
with `--latency-rtt 0`, `40`, `100`, `200` for comparable early gameplay. Uniform
jitter is 0/5/10/20ms per direction. Each gameplay profile measures ten wall-clock
seconds, both peers' FPS and simulation progress (`game_seconds` / wall seconds),
input rate, frame-arrival gaps and one observed shot response per player. The
optional soak continues simulation/hash validation, including death/menu transitions.
`--latency-test` without a selected RTT runs all four profiles on fresh game pairs.
The harness asserts 95–105% gameplay speed on each peer and checks that a later
client-only 60 FPS cap preserves game speed (90–110% during the transition).
Shot latency
includes harness observation overhead. These latency-only profiles add no packet
loss or bandwidth limit.
The client starts with unrelated personal save bytes: the test verifies gameplay
using the host snapshot, unchanged personal bytes and deleted session storage.
`python -m uctest.netem` self-tests relay ordering, delay, errors and cancellation.

Unstable-network checks (from `scripts`, each on a fresh pair):

```powershell
python -m uctest.netplay --speed 1 --network-stress spike --different-saves
python -m uctest.netplay --speed 1 --network-stress loss1 --different-saves
python -m uctest.netplay --speed 1 --network-stress loss5 --different-saves
```

These profiles use RTT100ms with ±10ms jitter per direction. `spike` adds 200ms
per direction for one second every eight seconds, starting three seconds into
the stress phase. `loss1`/`loss5` preserve every TCP byte but model head-of-line
recovery stalls: 1%/100ms and 5%/200ms respectively. They do **not** drop IP packets.
The stress phase lasts 30 seconds after the normal control and latency checks;
the report retains impairment counters and elapsed time even when the game exits.

Actual packet loss requires an explicitly owner-approved Administrator run and
the official WinDivert2.2 x64 DLL with its signed driver beside it:

```powershell
python -m uctest.netplay --speed 1 --latency-rtt 100 --different-saves --ip-loss 1 --windivert-dll "<absolute path to WinDivert.dll>"
```

Repeat with `--ip-loss 5`. Opening WinDivert can install/load its kernel driver.
Normal tests never load it. The filter is restricted to IPv4 loopback TCP with
both addresses 127.0.0.1 and source or destination equal to the test host port
(default27889). During the stress phase it drops 1%/5% of data packets, delays
all remaining packets including ACKs by 50±10ms in each direction, and disables
the relay's application delay. TCP therefore experiences the emulated RTT when
recovering losses. The bounded packet queue preserves order per direction;
the filter and both workers close on success or failure. The driver service may
remain installed after handles close. Reports distinguish these IP tests from
the synthetic TCP recovery model. No bandwidth limit is imposed.

Before pipelining, measured on 2026-10-23, report
`build/autotest/netplay-1792765049728219000/report.json`:

| Added nominal RTT | Jitter per direction | Simulation / real time | Observed shot response, P1 / P2 |
| --- | --- | --- | --- |
| 0ms | 0ms | 72.8% | 33 / 22ms |
| 40ms | ±5ms | 12.6% | 96 / 156ms |
| 100ms | ±10ms | 6.5% | 310 / 265ms |
| 200ms | ±20ms | 3.7% | 219 / 432ms |

Forty matching memory hashes through input frame4680; no desync. Different client
personal saves remained unchanged and session storage was removed. The zero-delay
row is the two-process, same-machine relay baseline, not a standalone-game FPS test.
These measurements reproduced the old per-sample request/response slowdown.
The pipelined protocol removes that network round trip from each simulated frame.
After pipelining and shared game-frame timing, measured on 2026-10-23:

| Added RTT | Jitter per direction | Host / client FPS | Host / client game speed | Frame-gap p95 |
| --- | --- | --- | --- | --- |
| 0ms | 0ms | 73.9 / 74.7 | 0.999 / 1.009 | 20.3ms |
| 40ms | ±5ms | 73.8 / 74.3 | 1.000 / 1.006 | 19.3ms |
| 100ms | ±10ms | 73.1 / 73.7 | 0.999 / 1.007 | 19.4ms |
| 200ms | ±20ms | 75.9 / 76.2 | 1.003 / 1.007 | 18.9ms |

Reports in `build/autotest/`, respectively:
`netplay-1792770097355934200`, `netplay-1792769855469833200`,
`netplay-1792769990772364300`, `netplay-1792770212974695700` (`report.json`).
Both harness guest threads shared CPU26 for the table above. These dual-process figures measure the
effect of injected network latency under the same contention; they do not predict
absolute FPS on two separate PCs. A warm zero-delay repeat is included above.
Network delay no longer reduces simulation speed or introduces a progressive FPS
drop within these profiles. Baseline frame-time spikes remain: maximum measured
frame gaps were 31.8–38.4ms across the four runs.

The 200ms run continued for another 60 seconds: 97 matching memory checks through
input frame11520, no desync, and 0.999 game speed during the client-only 60 FPS
test. An independent 200ms/60s repeat also passed with 98 matching checks through
frame11640 (`netplay-1792769613503271700`). Every profile passed independent
controls, disconnect, startup mismatch and temporary-save isolation checks.
Observed shot responses remain 0.40–0.54s at this frame rate: the fixed 32-frame
buffer is noticeable. Packet-loss behavior remains unmeasured.

A later wrapper verification (`netplay-1792770828301683100`) exhausted the input
buffer at frame652 in the menu. The client also recorded much slower shader
warm-up in that run; the exact cause of its stall is unconfirmed. This demonstrates
the finite buffer limit: the session terminates on a large peer stall. The passing
profiles above do not establish uninterrupted operation under arbitrary CPU/GPU
stalls or background load.

After the owner authorized separate physical cores and successful-run screenshots
were removed, fresh-pair results on CPU26/28:

| Added RTT | Jitter per direction | Host / client FPS | Host / client game speed | Frame-gap p95 / maximum |
| --- | --- | --- | --- | --- |
| 0ms | 0ms | 102.5 / 102.4 | 1.001 / 1.000 | 12.4 / 55.7ms |
| 40ms | ±5ms | 108.6 / 108.6 | 1.001 / 1.001 | 11.2 / 15.0ms |
| 100ms | ±10ms | 113.9 / 113.9 | 1.000 / 1.000 | 10.7 / 19.2ms |
| 200ms | ±20ms | 117.5 / 117.4 | 1.001 / 1.000 | 10.3 / 18.1ms |

Reports, respectively: `netplay-1792772181993016100`,
`netplay-1792772438024505400`, `netplay-1792772526602157900`,
`netplay-1792772602939750600`. The zero-delay run had frame logging enabled;
the final three ran without it. These are live fights, so changing input timing
and machine load can vary the workload; the table is not a deterministic speedup
benchmark. Final shot-response observations were 0.29–0.32s.
The final 200ms `.bat` completed its 60-second soak with 104 matching memory checks
through input frame 12360; client 60 FPS retained 0.999 game speed. Both independent
controls, save isolation/cleanup, disconnect and startup rejection passed.

An earlier separate-core 200ms run passed but had 94.4 FPS, p95 21.3ms and a 70.9ms
maximum (`netplay-1792771989530407400`). A further 200ms repeat measured 113.7 FPS,
p95 10.6ms and 13.5ms maximum (`netplay-1792772273571847100`). The variability and
zero-delay outlier mean that occasional local hitches remain possible; full-memory
validation and a finite input buffer remain enabled. The final 60 FPS phase is
intentional and explicitly announced in the console; afterwards both games exit.

After startup delay negotiation (UC-NET-9), fresh CPU26/28 pairs measured:

| Added RTT | Jitter per direction | Input buffer | Shot response P1 / P2 | Host / client FPS | Host / client game speed | Frame-gap p95 / maximum |
| --- | --- | --- | --- | --- | --- | --- |
| 0ms | 0ms | 7 frames | 79.3 / 77.7ms | 117.2 / 117.2 | 1.001 / 1.001 | 10.1 / 12.5ms |
| 40ms | ±5ms | 10 frames | 101.8 / 92.3ms | 116.8 / 116.8 | 1.000 / 1.000 | 10.5 / 15.7ms |
| 100ms | ±10ms | 14 frames | 133.8 / 133.3ms | 117.4 / 117.3 | 1.001 / 1.000 | 10.4 / 17.5ms |
| 200ms | ±20ms | 21 frames | 191.3 / 189.0ms | 116.0 / 116.0 | 1.001 / 1.001 | 10.5 / 14.6ms |

Reports: `netplay-1792773857450431500`, `netplay-1792773914879642400`,
`netplay-1792773973044995900`, `netplay-1792773711172737100`.
Both peers agreed on the measured maximum RTT and buffer in every report.
The 200ms run included a 60-second soak: 101 matching memory hashes through
input frame 12000; client-only 60 FPS retained 0.999 game speed. Relative to the
previous 32-frame 200ms run, shot response fell from 286.2/289.9ms to 191.3/189.0ms
(33–35%), with similar live FPS (117.5/117.4 versus 116.0/116.0).
All profiles passed independent controls, save isolation/cleanup, disconnect and
incompatible-start rejection. Local 46.9/84.5ms work hitches occurred outside the
200ms measurement window; occasional local stalls remain possible.
A fresh 200ms repeat (`netplay-1792774033464326100`) negotiated 20 frames from
232.6ms measured RTT: 183.6/175.2ms shot response, 116.9/117.0 FPS, game speed
1.000/1.001, frame-gap p95/max 10.4/13.2ms, 41 matching hashes through frame 4800.
The buffer is selected only at startup: a later sustained latency increase beyond
its budget ends the session. Packet loss and real WAN traffic remain unmeasured.

The deterministic offline `fight2` benchmark replayed three alternating pairs:
2,608,931 draws in every run; mean wall time15.710s before versus15.417s after.
The paired change was−1.9%,95% CI[−9.7%,+6.0%], so no statistically established
speedup. This checks unchanged offline work; the live pairs above measure network
behavior. Full CTest175/175 and harness tests12/12 passed.

Launcher UI verification:

```powershell
.\dist\uc_launcher.exe --smoke build/autotest/launcher-network
```

The built-in smoke opens the modal, exercises invalid IP/port and Cancel, creates
a real loopback lobby, clicks Ready, proves one ready player cannot start, then
checks the exactly-once handoff after both are ready. It intercepts that handoff
before starting a game. `test_launch` separately verifies real child-process
arguments; the game scenario above verifies the two actual game processes.
The modal uses a 28-unit title above 22-unit body text, 20-unit inner padding and a
single bottom action row with Cancel aligned right. Smoke captures include the
initial form, invalid endpoint and connected/readiness state.
Ready removes the old vertical frame padding to halve its square while retaining
the normal text size; an8-unit inner gap separates label and square. Multiplayer
and Singleplayer share the former Play area equally, separated by an8-unit gap.
Final UI verification: `build/launcher-play-buttons.log`, screenshots under
`build/autotest/launcher-play-buttons`. Locale and launch tests also passed after
the final button relocation; the game executable was unchanged by these UI edits.

Verification scope: two processes on this Windows machine, including injected
Internet-like latency/jitter. Separate computers, actual Internet connections
and complete campaign playthroughs require additional testing.

## Unstable-network results (2026-10-23)

The requested uninterrupted-play criterion **failed**. Fresh harnesses used
separate physical cores, copied saves, unlocked FPS and RTT100ms ±10ms jitter.
Before impairment the final two pairs measured117.2/117.2 and119.24/119.14 FPS,
with game speed1.001/1.001 and1.001/1.000 respectively.

| Impairment | Observed result | Matching memory checks before exit |
| --- | --- | --- |
| Actual1% data-packet loss, IP data/ACK delay50±10ms each way | Buffer exhaustion; test ended after0.829s,2 dropped and112 delayed packets | 36 through frame4200 |
| Actual5% data-packet loss, same IP delay | Buffer exhaustion; test ended after1.294s,4 dropped and130 delayed packets | 36 through frame4200 |
| Application RTT burst100→500ms for1s | Buffer exhaustion at frame4606; test ended3.819s into stress, burst began at3s | 39 through frame4560 |

Reports: `netplay-1792776564616640600`, `netplay-1792776610323501100`,
`netplay-1792775659264694700`, respectively. No hash mismatch was observed before
termination. Stress phases ended too early to establish sustained FPS or game
speed under impairment. The driver reported no capture/reinjection errors.
Loss percentages are configured probabilities, not exact fractions in these
short failed samples. The input buffer remained14 frames; late TCP input causes
controlled exit with code5. This still violates the no-exit requirement.

Earlier actual-loss probes delayed only the relay's application stream, leaving
TCP recovery on local RTT. Those passed30s with75/388 dropped packets,
116.95/117.74 FPS,1.000 game speed and68 matching checks
(`netplay-1792776275415542700`, `netplay-1792776365611174100`). They are useful
local-loss checks but **do not establish Internet-loss robustness**. The final
IP/ACK-delay tests above supersede that interpretation. Separate synthetic
recovery-stall profiles also failed:1%/100ms after0.545s and5%/200ms after0.933s
(`netplay-1792775602453661500`, `netplay-1792776031473188000`).

No production-network workaround was applied. The fixed lockstep buffer cannot
advance missing input indefinitely: more buffering increases input latency,
waiting interrupts progress, and prediction requires state correction/rollback.
A complete rollback/savestate path is not currently implemented.

The Multiplayer button now inherits the normal graphite button palette;
Singleplayer retains red styling. Native launcher smoke passed including lobby
validation, Ready gating and cancellation (`build/launcher-multiplayer-gray.log`).
The verified launcher was deployed to `S:\Games\Umbrella Chronicles`;
SHA256 `828F98BD9EB855693A43CAC5A16F248D3AA61B8FD4B143F32060661D6AC53BFC`.
Harness unit checks passed15/15; the relay self-test passed. After the runs,
the temporary WinDivert driver was stopped and its service was confirmed absent.
No game or impairment process remained running.

## Rollback preparation and repeated failures (2026-10-23)

Rollback is **not implemented or enabled**. The new8003F670 outer-iteration
boundary provides a returning game iteration, preserving the original backward
branch poll. Its unit test executes20000 iterations without native stack growth;
the final build passed176/176 CTest tests.

The fixed fight2 benchmark performed2608931 draws, identical per-retrace work,
in all6 runs. Baseline/new mean wall time15.830/15.209s; paired change-3.7%,
95% CI[-17.7%,+10.3%]: inconclusive because the first baseline run was slower.
Best-vs-best change-0.2%; peak FPS173.8/174.9. This is an offline regression
check of the boundary, not a rollback performance result.

A temporary boundary census observed8 live guest threads during the fight.
Other threads remain suspended inside native SDK calls, message waits and VI
waits. Neither guest RAM nor the main-thread context contains those continuations.
The diagnostic hook was removed after capture. Full scheduler/HLE continuation
restoration, output fencing and input prediction remain outstanding.

Repeated CPU26/28 harness runs retained the same RTT100ms and±10ms jitter:

| Stress profile | Result | Actual dropped / delayed IP packets | Matching hashes before exit |
| --- | --- | --- | --- |
| 1% actual loss, data/ACK IP delay | Buffer exhaustion after1.022s | 2 /118; no filter error | 36 through4200 |
| 5% actual loss, data/ACK IP delay | Buffer exhaustion after1.004s | 2 /86; no filter error | 35 through4080 |
| Application RTT burst100→500ms at3s | Buffer exhaustion after3.903s | No IP-loss injection in this profile | 39 through4560 |

Reports in build/autotest:netplay-1792779407729083200,
netplay-1792779453602537400,netplay-1792779499285079900. All3 stress tests failed.
Zero observed hash mismatches before exit does not establish sustained synchronization.
The no-loss measurement windows preceding stress ran112.9–119.5FPS at1.000–1.001
simulation speed; measured shot responses128.8–138.3ms. These figures do not
describe performance during loss. No low-latency rollback claim is supported.

WinDivert was stopped and its service was confirmed absent after the runs.
The experimental game build was staged only to repository dist; no rollback
build was deployed into the owner's game folder.

## Experimental rollback status (2026-10-23, later runs)

Rollback is now implemented experimentally: thread/HLE restoration, predicted
remote input, replay, redundant UDP input and independent TCP heartbeat. UC-NET-11
encodes gameplay input in108 bytes plus11 bytes per event. Presentation limits
and replay no longer stop processing the guest's GPU packets; hidden presents
retain EFB/copy/clear/completion semantics.

These results supersede the earlier buffer-exhaustion behavior, but do not yet
establish the requested stable performance:

| Run | Actual impairment | Observed result |
| --- | --- | --- |
| netplay-1792785434541013800 | RTT100ms, jitter±10ms,1% IP loss | 462 UDP drops;30 matching checkpoints;30s completed; maximum frame gap58ms |
| netplay-1792785785431912500 | RTT100ms, jitter±10ms,5% IP loss | 2232 UDP drops;30 matching checkpoints;30s completed; maximum frame gap60.7ms |
| netplay-1792785881042739300 | Planned500ms RTT bursts | Preliminary no-loss check failed at114.92/111.92FPS and0.958/0.933 simulation speed; bursts were not reached |

The successful loss runs measured approximately121FPS during the30s stress
interval, including catch-up after earlier slower work. Average speed alone is
insufficient: reports now expose one-second simulation windows. Both passed
save isolation, temporary-save removal, startup mismatch and disconnect checks.
The1% run predates the binary codec; the5% run includes it. Hash equality currently
covers RAM and canonical scheduler/register state, not every HLE field.

CPU26/28 baseline runs also reproduce late-fight110–113FPS and0.91–0.95 simulation
speed without impairment. The asymmetric FPS ceiling checks now require0.98–1.02
speed. Actual uncapped presentation in these runs is near120FPS; setting a200FPS
ceiling does not demonstrate200 rendered frames per second. Full independence
from rendering cost, burst recovery smoothness and low-latency behavior still
require validation. No experimental rollback build has been deployed to the
owner's game folder. Scoped WinDivert services were cleaned up after these runs.

The later real burst run `netplay-1792787390602471200` reached the impairment:
RTT100ms with ±10ms jitter, 1% loss and +200ms one-way bursts. It dropped379
UDP packets and completed30s with23 matching confirmed checkpoints. Performance
failed: host/client93.03/93.09FPS, simulation0.777/0.776, maximum observed gaps
263.8/368.9ms. Every measured one-second window was below0.95 speed. These
numbers came from the rejected refreshed-PGO `dist/uc.exe`: UAC did not preserve
the intended executable-selection environment variable. They must not be
attributed to the older profile or directly compared as a network-only change.
The elevated runner now takes an explicit executable argument and records its
resolved path. Driver cleanup succeeded; saves/disconnect checks completed.

Source investigation confirms that netplay uses the deterministic GPU completion
path, whereas ordinary play can release GXWaitDrawDone at command submission.
This serializes some guest work with renderer/GPU completion. The boundary wall
deadline is cumulative; it does not unconditionally add an8.33ms sleep after
checkpoint work. The contribution of GPU completion waits to the remaining FPS
loss is not yet isolated. No production change to completion ordering has been
made: guest-visible event ordering and rollback correctness need verification.

## Detached rendering measurements (2026-10-24)

The earlier experimental scene-history expansion repeatedly resubmitted old GPU
work and could exhaust memory. It was removed. Capture now retains only the
original commands of one logical frame and a single newest-frame mailbox.
Logical GX completion and checkpoint restoration passed without a renderer;
physical copy/clear/feedback behavior passed `renderer_rollback_smoke`.
Mesh cache entries survive restore but revalidate descriptor/source bytes, including
RAM restored without a write barrier. Actual-input frames no longer allocate
unnecessary checkpoints; asynchronous completion draining remains in place.

Latest real WinDivert run: `build/rollback-stress-1792792551583833500.log`.
Each profile used two1280x720 harnesses on CPU26/28, RTT100ms with ±10ms one-way
jitter,30s of active controls and real TCP/UDP packet impairment. Burst profile
adds200ms per direction for one second every eight seconds.

| Profile | Dropped UDP | Host/client FPS | Forward game speed | Native p99 / worst frame, host; client | Result |
| --- | ---: | --- | --- | --- | --- |
| 1% loss | 454 | 120.04 /120.04 | 1.000 /1.000 | 11.0 /43.8ms;11.0 /16.3ms | Passed |
| 5% loss | 2223 | 120.04 /120.04 | 1.000 /1.000 | 10.5 /30.8ms;10.1 /14.6ms | Failed: one client1s window0.945× |
| 1% loss, RTT100→500ms bursts | 471 | 117.87 /117.89 | 1.000 /1.000 | 16.8 /190.4ms;15.3 /184.3ms | Failed: correction stalls and1s speed windows |

All three completed, with30 matching confirmed checkpoints during each stress
window and no observed desync or crash. Average1× speed does not establish smooth
play: burst windows ranged0.623–1.663×. Separately,200/30 and30/200 presentation
ceilings retained1× simulation; the higher-ceiling peer actually rendered near120FPS.
Save isolation, temporary-save removal, mismatch rejection and disconnect checks
completed. Scoped WinDivert cleanup succeeded.

Harness telemetry now timestamps samples at their source and exposes forward game
progress separately from the rewindable replay clock. Reports retain1s window
endpoints and read every native presentation interval from `frames.csv` after exit.
Acceptance budgets are0.95–1.05× per1s window, native p99≤16.7ms and max≤50ms.
Missing/stale measurements fail; an acceptable long-term average cannot conceal a
250ms pause. These budgets are test criteria, not a claim of perfectly constant FPS.

Rollback remains experimental. CPU replay still executes guest rendering callbacks
whose state writes prevent skipping the entire pass safely. Profiling measured
restore around1ms, with22 replayed iterations taking roughly210–236ms before the
checkpoint/cache optimizations. Large-correction smoothness remains unfinished;
The following deployment note records the later owner-requested copy.

Follow-up verification: the full detached-render build passed183/183 tests. Three
paired offline `fight2` runs measured16.206s baseline and16.135s current wall time,
with identical2608931 draws; difference−0.4%,95% CI[−5.0%,+4.1%], inconclusive.
Logs: `build/rollback-detached-full-build.log`, `build/rollback-detached-bench.log`.
The eight-iteration actual-game restore/replay probe also passed on this version:
`build/autotest/rollback-probe-1792793574335817100`.

Review found that the render-side latest-copy index retained abandoned addresses.
It now holds weak references; active aliases and GPU slots retain required versions.
The GPU fixture verifies that an abandoned copy expires after in-flight slots retire,
while restored aliases still display their original pixels. Finishing an already
selected predicted frame remains intentional: dropping its remaining commands would
leave a partially updated EFB. The next selected frame reflects corrected simulation.

Input capture previously stopped throughout replay and caused additional late input
at the peer. A monotonic wall-time sampler now continues at replay boundaries and
through catch-up, without overwriting sent frames. Missed sample slots retain the
preceding held state; new input edges belong only to the latest sampled slot. The
sampler and outgoing history are intentionally outside rollback snapshots.

Intermediate profiled burst run `netplay-1792793862169298000` dropped413 UDP packets,
matched30 confirmed checkpoints and completed functional/save/disconnect checks.
Host/client averaged119.94/119.67FPS and1× simulation, with7/3 corrections and44/20
replayed frames. Native p99 was10.57/9.81ms, worst160.89/137.21ms; the strict test
still failed. Baseline shot response was135.8/142.4ms with a10-frame input pipeline
(startup maximum RTT124.8ms). These are one run's measurements, not a worst-case
latency guarantee. WinDivert cleanup succeeded. Final unprofiled reruns follow below.

### Final unprofiled rerun of this increment

`build/rollback-stress-1792794246173570800.log` tested the final sampler, including
missed-slot handling, with the same impairment parameters and CPU26/28.

| Profile / run suffix | Dropped UDP | Host/client FPS | Average speed | Native p99 / worst, host; client | Acceptance |
| --- | ---: | --- | --- | --- | --- |
| 1%,1792794246177955100 | 462 | 120.08 /120.04 | 1.000 /1.000 | 12.0 /14.7ms;9.9 /13.5ms | Passed |
| 5%,1792794343691778500 | 2196 | 120.05 /120.02 | 1.000 /1.000 | 10.1 /14.6ms;11.0 /57.6ms | Failed: one client frame |
| Bursts,1792794440389545200 | 457 | 119.80 /119.15 | 0.998 /0.993 | 10.5 /121.9ms;10.6 /195.7ms | Failed: frame gaps, speed windows and catch-up |

The stress windows matched30/30/29 confirmed checkpoints respectively; no crash
or mismatching checked hash occurred. All functional/save/disconnect checks completed.
The5% interval had no rollback on either peer; its isolated57.6ms client gap had
0.53ms GPU time,0.46ms renderer work and57.76ms guest-thread wall time. Its exact
guest-side cause is not established; it is not dismissed as measurement noise.

The burst run had10/17 corrections and54/67 replayed iterations, maximum depths
13/21. Worst1s speed ratios were0.879/0.794. Immediately afterwards, the client
caught up at1.037× during the200/30 cap phase, which correctly failed its0.98–1.02
gate. After settling,30/200 retained1× on both peers. Both cap directions passed
for1% and5% loss. Actual high-ceiling rendering remained near120FPS, not200FPS.
Baseline shot-response observations across these runs ranged107.9–166.7ms; these
samples do not bound input delay during the failed burst corrections.

The final full build passed183/183 tests(`build/rollback-input-clock-full-build.log`),
and the harness suites passed20 tests. GPU copy lifetime and restored-feedback
smoke passed. The scoped input-sampling review found no remaining issue after the
missed-slot fix. WinDivert cleanup succeeded and all harness processes exited.
The owner subsequently requested deployment. The previous increment was copied
to `S:/Games/Umbrella Chronicles` on2026-10-24, with11 changed files backed up in
`build/deploy-backup-20261024-013908`; saves/config/mods stayed untouched. Its
`uc.exe` SHA256 is`2E10F1351B87D2A9EE8A941AEA6390761127F728029E381024D300F729A5B7A2`.
This deployment did not resolve the remaining rollback smoothness failures.

### Replay CPU cost and graphics assembly

`UC_ROLLBACK_PROFILE` now reports individual replay-step wall time, main-thread
CPU cycles, guest pacing wait and scheduler wake delay, separately from boundary
work. In `netplay-1792796087657383200`, heavy replay steps took median7.421/7.561ms
and32.847/33.382Mcycles(host/client). Pacing/wake waits were roughly0.1ms; one
14-step correction took114ms. The main cost is actual guest execution, not a
network or GPU sleep. Removing only the non-checkpoint `drain_async` is unsafe:
peers with different prediction histories must retain identical external-event
delivery barriers.

Replay now omits host material/texture/draw-packet assembly in live primitives,
recorded display lists and native meshes. Geometry/resource validation, original
guest callbacks, RAM writes, matrix loads, copies and logical GX completions still
execute. Selecting the SDK block is retained even when material assembly is
skipped, because later raw XF commands write texgen words into that block.

The comparable profiled run `netplay-1792796608297129100` measured heavy replay
medians6.722/6.886ms and29.720/29.970Mcycles: about9–10% lower per-step cost.
Correction depths differed, so these are diagnostic samples, not a paired scene
benchmark. Worst present gaps still reached136.6/126.0ms. The change is a partial
improvement, not completion of the no-pause requirement.

The actual eight-step replay probe passed after the final SDK-block fix:
`build/rollback-render-skip-final-probe.log`. The full build passed183/183 tests;
five focused GX/mesh/EFB tests passed after that fix. Three paired offline
`fight2` runs measured16.170s baseline vs16.216s current,+0.3%,95% CI[-1.0%,+1.6%],
identical2608931 draws(`build/rollback-render-skip-bench.log`). No material offline
regression was measured. The last SDK fix only changes replay branches.

The harness additionally records send-to-consumption-reply latency for input
commands during stress, including fire-and-forget commands. This includes harness
IPC/reply delivery, requested hold duration and consumption of the release. It does
not measure initial press-to-photon or confirmed remote state.
Rejected and still-pending commands are reported separately. Harness tests22/22
pass(`build/rollback-render-skip-harness-tests.log`).

### Unprofiled real-loss verification after graphics assembly changes

All runs below use CPU26/28, 30s stress, RTT100ms with10ms jitter and real
WinDivert drops. The burst profile adds200ms one-way delay periodically (RTT500ms).

| Profile / run suffix | Dropped UDP | Host/client FPS | Average speed | Native p99 / worst, host; client | Acceptance |
| --- | ---: | --- | --- | --- | --- |
| 1%,1792797293767619900 | 447 | 120.07 /120.07 | 1.000 /1.000 | 10.4 /20.4ms;10.9 /17.5ms | Passed |
| 5%,1792797390057016800 | 2190 | 120.03 /120.04 | 1.000 /1.000 | 10.2 /22.9ms;10.9 /14.4ms | Passed |
| Bursts,1792797667081753400 | 439 | 119.97 /120.00 | 1.000 /1.000 | 10.0 /128.7ms;10.5 /112.5ms | Failed: frame gaps and speed windows |

Each stress interval matched30 confirmed RAM/scheduler hashes; no crash or
mismatching checked hash occurred. Functional checks completed. Asymmetric render
caps retained1x speed. These hashes do not cover every HLE callback payload.

The earlier burst run `netplay-1792797486721422500` is excluded from the clean
comparison: both renderers repeatedly blocked in presentation for roughly500ms.
The owner confirmed resuming another demanding game and enabling a hard frame
limiter, then confirmed disabling the limiter and freezing that game for the
repeat. The repeated run has no500ms stall, but still has three frames over50ms
per peer and minimum1s speed ratios0.863/0.881 (maximum1.100/1.067 during catch-up).
It performed13/9 corrections,67/56 replay iterations, maximum session depth14.

Stress input send-to-consumption-reply latency, host/client:

| Profile | Median | p95 | Maximum |
| --- | --- | --- | --- |
| 1% | 149.6 /152.7ms | 167.6 /178.1ms | 210.3 /209.9ms |
| 5% | 158.3 /116.0ms | 192.2 /133.8ms | 208.6 /138.7ms |
| Bursts, repeat | 171.2 /171.5ms | 289.3 /280.4ms | 324.7 /294.8ms |

No rejected or pending input commands remained. Startup negotiated11 buffered
frames in the repeated burst run (maximum measured RTT140.2ms). Average120FPS
and1x speed do not satisfy the no-pause or low worst-case input-latency requirement.
Remaining correction cost is CPU replay; this increment is not a completed fix.
The repeat helper log is `build/rollback-stress-1792797667075688400.log`; its
done JSON confirms WinDivert cleanup. The current executable is staged in `dist`;
the earlier owner-requested game-folder deployment remains the previous version.

### Input queue and burst repair follow-up

The configured input delay was not the actual queue depth: the profiled run
`netplay-1792841671592992100` negotiated 9 frames, but had 13 queued before stress,
14 after a correction and peaks of 21. Forward recovery now consumes already
sent frames before capturing more; replay still captures on wall time. Sent
frames remain immutable and pending button edges survive the drain. Profiled
checks show the lead returning to its configured 9 or 10 frames.

Startup uses the upper quartile of nine RTT probes, with the existing two-frame
margin, instead of permanently sizing the buffer from a single outlier. Replay
validates vertex addresses without converting unused host vertex values or
allocating geometry. A different QTE window marker only triggers correction
when the input contains a shake action, whose filtering depends on that marker.

A profiled intermediate spike run (`netplay-1792842198399300200`) disconnected
after exhausting the two-second history. Its generic diagnostic did not identify
which progress counter stalled. The failure now reports all four counters. The
transport also fixes three independently identified weaknesses: skipped input
slots get an initial send, up to eight old missing slots are repaired together,
and confirmed hashes get a reliable TCP copy. The history limit is unchanged.

Final unprofiled tests use the same two harness, cores 26/28, 30-second stress,
100 ms RTT, 10 ms one-way jitter and real WinDivert packet drops. Spikes add
200 ms one-way delay periodically. Reports are under `build/autotest/netplay-*`.

| Profile / run suffix | UDP drops | Host/client FPS | Native p99; maximum, host / client | Result |
| --- | ---: | --- | --- | --- |
| 1%, 1792842887176045600 | 958 | 120.03 / 120.03 | 11.0 / 10.9; 16.5 / 16.0 ms | Pass |
| 5%, 1792842983033460200 | 4782 | 120.03 / 120.04 | 10.2 / 9.8; 13.4 / 14.1 ms | Pass |
| Spikes, 1792843078170312300 | 999 | 119.91 / 119.95 | 12.9 / 10.6; 154.4 / 167.7 ms | Fail: pauses and speed windows |

All three runs complete functional checks without a crash or mismatching checked
hash (30 confirmed stress hashes each). These hashes cover RAM and canonical
scheduler state, not every HLE callback payload. Average simulation speed is 1x
for both peers. Asymmetric 200/30 and 30/200 render caps retain approximately 1x
speed. The high-cap peer actually renders around 120 FPS in this scene.

| Input send-to-consumption-reply | Median, host / client | p95 | Maximum |
| --- | --- | --- | --- |
| 1% | 107.6 / 107.4 ms | 125.3 / 125.2 ms | 135.0 / 126.6 ms |
| 5% | 107.9 / 110.2 ms | 125.2 / 127.1 ms | 126.5 / 131.6 ms |
| Spikes | 107.6 / 107.7 ms | 259.5 / 242.3 ms | 304.4 / 293.4 ms |

No rejected or pending commands remain. This metric includes IPC, the requested
hold and release consumption; it does not measure initial press-to-photon. Each
run negotiated 10 buffered frames (83.3 ms). Compared with the previous clean
spike run, median command completion improves from about 171 ms to 108 ms; worst
pauses remain unacceptable and did not improve. Minimum one-second speed during
spikes is 0.804 / 0.808, with catch-up peaks of 1.160 / 1.149. Each peer has eight
native present intervals over 50 ms. CPU replay remains the limiting factor.

Helper `build/rollback-stress-1792842887171512300-done.json` confirms cleanup.
The exact intermediate disconnect did not recur in the final runs; this bounded
test does not prove it impossible. The actual eight-step restore/replay probe
passes with equal RAM, thread contexts and virtual clock
(`build/rollback-input-drain-probe.log`). The executable is staged in `dist`;
the game-folder deployment is unchanged.

Final CTest: 183/183 pass (`build/rollback-input-drain-final-ctest.log`). Harness
network/emulation tests: 22/22 pass (`build/rollback-input-drain-harness-tests.log`).
An initial serial paired benchmark measured 15.777 s baseline versus 22.569 s
current (+43.0%); the reverse-order repeat measured 15.922 s current versus
15.802 s baseline. No code or binary changed between these runs. The anomalous
slow run remains recorded; its cause is unconfirmed. The earlier three-pair
vertex-only measurement overlapped a build and is excluded as clean evidence.

The final serial three-pair benchmark alternated execution order and measured
15.811 s baseline versus 15.932 s current: +0.8%, 95% CI [-0.1%, +1.6%],
inconclusive. All six runs performed 2,608,931 draws. Peak FPS was 164.3 versus
164.0. The 43% slowdown did not recur in these repeats
(`build/rollback-input-drain-final-bench.log`). This offline comparison does not
change the failed burst-smoothness result above.

The owner subsequently confirmed that the other demanding game remained frozen
and the external limiter was disabled during these measurements. The isolated
43% slowdown therefore remains unexplained; do not attribute it to those two
conditions. It did not recur in the reverse-order or final three-pair repeats.
