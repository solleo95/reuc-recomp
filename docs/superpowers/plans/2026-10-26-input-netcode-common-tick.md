# Input-packet netcode with a common tick Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cooperative play where only input travels over the network, each PC draws at its own pace, and a weak PC lowers a shared simulation tick instead of crashing or dragging the other PC's frame rate below that tick.

**Architecture:** Return to the committed rollback netcode (`HEAD:game/netplay_game.cpp`, deterministic virtual clock, UDP input + TCP hashes), drop the host-authoritative video stream. The simulation tick is a session value (60 or 30 Hz, the port's two native logic rates), chosen automatically and changed only at an agreed frame number. The faster PC never runs further ahead than the rollback history allows (lead throttle), so a slow peer degrades game speed gracefully instead of ending the session. The host's displayed rate above the tick comes from the existing frame generation.

**Tech Stack:** C++20 (clang-cl), Vulkan renderer, existing `uctest` harness (Python), Media Foundation not needed any more.

**Spec:** this conversation (owner, 2026-10-26): no video; host at its own FPS; weak client must not limit the host's FPS; public project, any GPU; automatic parameters; low input lag. Prior design notes: `docs/testing/netplay.md` (historical rollback sections), `build/backup-host-auth/` (video version).

## Global Constraints

- Only input and hashes cross the network: no video, no state transfer; LAN bandwidth stays in the kilobyte range.
- Works on any GPU vendor; frame generation is optional per machine and never changes the simulation.
- Simulation stays deterministic: identical dt sequence on both PCs; tick changes only at an agreed frame.
- Never run any test fullscreen (harness forces 1280x720); few live runs; edit source only with Edit/Write.
- End state: full `build.bat --no-install` passes, deploy `uc.exe`, `data/uc.map`, `release.json` to `dist-raw`, `dist-release` and `<laptop share>` with hash check (recipe in `build/h264-deploy/install.ps1`).

## Review Focus

- Peer much slower than the tick for minutes: session must continue (reduced game speed or tick downshift), never exit with "input sampling exceeded rollback history".
- Tick change while a rollback replay or a pause is in progress: both peers must apply it at the same frame and stay hash-equal.
- Client joins with a different GPU/CPU class (Intel iGPU, no frame generation): game starts, host's frame generation does not desync it.
- Peer disconnect or ESET-blocked UDP: clear error, no hang.
- Laptop with a slow disk/shader warm-up delaying start: startup barrier still aligns both clocks.

---

### Task 1: Restore the input-packet netcode, remove the stream

**Files:**
- Modify: `game/netplay_game.cpp`, `game/netplay_game.h` (HEAD implementation plus the helpers the working tree added: `merge_net_input`, `net_frame_period` only if still used, `NetInputClock::sample(...,period)`), `game/uc_main.cpp` (undo stream hunks: `run_remote_client` early return, `set_stream_capture`, audio submit, `take_stream_frame`, `Packets{bool(network)}`, `fps_limit` `!network` guard, render drain `break`), `game/options.cpp` (netplay branch: `deterministic=true`, no `game_hz=120`), `game/CMakeLists.txt`, `tests/CMakeLists.txt`, `renderer/vulkan_renderer.{h,cpp}` (remove stream capture; keep `cached` readback allocation and its test), `docs/testing/netplay.md`.
- Delete: `game/remote_*.{cpp,h}`, `game/remote_video.cpp`, `tests/test_remote_session.cpp`, `scripts/uctest/remote_play.py`.
- Keep: launcher auto-connect/NAT UI, wait screen, diagnostics, compatibility work.

**Interfaces:**
- Produces: `NetGame` as in HEAD (`install`, `prepare`, `waiting`, `error`, `frames`, `simulation_ns`, `consumed_sequence`); no `remote()`.

- [ ] **Step 1:** Save the video tree: `build/backup-host-auth/` already holds the pre-H.264 patch; add `git diff HEAD > build/backup-host-auth/h264.patch`.
- [ ] **Step 2:** Re-apply HEAD's `NetGame::Impl` over the working-tree helper section; build `uc`, `test_netplay_frame`, `test_sched`; expect all pass.
- [ ] **Step 3:** Run `python -m uctest.netplay --speed 1 --latency-rtt 0` once (harness pair on loopback); expect hashes equal, 95-105% speed.

### Task 2: Common tick (60 or 30 Hz) in the session

**Files:** Modify `game/netplay_game.cpp` (hello field `tick_hz`, per-frame pacing), `game/options.cpp`, `game/game_pacing.{h,cpp}` (mode from tick: 60 = `{unlock_60:true,hz:0}`, 30 = `{false,0}`), `tests/test_netplay_frame.cpp`.

**Interfaces:**
- Produces: `uint32_t net_tick_hz` in `Options::netplay` (default 60; `--net-tick 30|60`); `net_input_delay(max_rtt_ns, tick_hz)` and `NetInputClock` period derived from the tick.

- [ ] **Step 1:** Test `net_input_delay(100'000'000,60)==2+uint32_t((100'000'000ull*60+1'999'999'999)/2'000'000'000)` and 30 Hz halves it; `NetInputClock::sample` with `period=1e9/60`.
- [ ] **Step 2:** Replace the 120 constants (`input_delay`, `period`, `confirmed/120`, hash interval 120) with `tick_hz`-derived values; both peers must agree through the `hello` JSON (mismatch rejects startup).
- [ ] **Step 3:** Live run at 60 and at 30: `uctest.netplay --speed 1` each; expect equal hashes and game speed 0.98-1.02.

### Task 3: Bounded lead and slow-peer behaviour

**Files:** Modify `game/netplay_game.cpp` (`boundary`), `tests/test_netplay_frame.cpp`, `scripts/uctest/netplay_slow_peer.py`.

**Interfaces:**
- Produces: `uint64_t net_lead_limit(uint64_t input_delay)` = frames the local sim may run beyond `remote_next` (default `input_delay + 6`); above it the sampler sleeps in 1 ms steps while receiving.

- [ ] **Step 1:** Test `net_lead_limit(8)==14`, and that a clock which has been throttled never produces a slot above `head+history_limit`.
- [ ] **Step 2:** In `boundary`, wait (cancellable, receiving UDP) while `cursor>remote_next+lead`; remove the "input sampling exceeded rollback history" exit path for this case.
- [ ] **Step 3:** `netplay_slow_peer.py`: client with `--render-scale 4` (heavy) must keep the session alive 60 s with hashes equal; host speed equals the client's speed (documented).

### Task 4: Automatic tick selection and agreed change

**Files:** Modify `game/netplay_game.cpp` (rate report in the acknowledgement JSON, `tick` control message with `at_frame`), `game/game_pacing.cpp` (apply at boundary `at_frame`), `tests/test_netplay_frame.cpp`.

**Interfaces:**
- Produces: `NetTickPolicy { uint32_t update(uint32_t current_hz,double host_rate,double peer_rate,double now_s); }` returning the wanted tick; downshift to 30 when either rate stays below 92% of the tick for 3 s, upshift to 60 when both exceed 140% of 30 for 15 s.

- [ ] **Step 1:** Policy tests (downshift, hysteresis, upshift, no flapping).
- [ ] **Step 2:** Host sends `{tick:30,at:F}` with `F=head+2*input_delay`; both apply when `cursor==F`; checkpoints already carry pacing state (`PacingSnapshot`).
- [ ] **Step 3:** Live: client with injected load drops to 30 and returns; hashes equal across the change.

### Task 5: Host frame generation above the tick

**Files:** Modify `game/uc_main.cpp`/`renderer` only if netplay gates it; `docs/testing/netplay.md`.

- [ ] **Step 1:** Run harness host with `--frame-generation 2` in a net session; check presents ≈ 2x tick and that the client hash stream is unchanged.
- [ ] **Step 2:** If detached rendering blocks it, record the cause and keep host rate = tick; do not hack around it.

### Task 6: Per-step cost

**Files:** profile first (`UC_FRAME_LOG`, `--profile`) at tick 60 in a net session; touch only measured hot spots; judge with `uctest bench --demo` (deterministic), not live FPS.

- [ ] **Step 1:** Measure guest step ms at 60 Hz: single-player vs net (checkpoint cost `game_checkpoint` every 120 frames or on prediction).
- [ ] **Step 2:** Remove net-only per-step overhead found; keep wins ≥1% that are correct (owner rule).

### Task 7: Verify and deploy

- [ ] `build.bat --no-install` (all tests), `python -m uctest.netplay` (60 Hz, latency 100 ms, loss profile), final review on Opus, deploy with the hash-checked script, report what was not measured on the real laptop.
