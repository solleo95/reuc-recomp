# Agent test harness — design

Date: 2026-10-15. Owner approved approach B (in-game test server) in conversation.

## Purpose

Agents test the native port without the owner: regression checks (reach a scene, capture frames, compare with references, check audio), control tests (every game action is accepted and has its effect), performance measurement (stable FPS/frame time in identical situations) and an autoplay mode that plays as far as it can (aims at enemy heads, shoots, reloads, handles QTEs and menus) while logging, to reach the next crash point or prove a stretch of the game completes. Free-form LLM-driven play is out of scope; autoplay is a scripted bot.

Success: a scenario such as `train_gameplay` reaches active gameplay 10 times in a row without a failure, faster than `scripts/train_route.py`, and every failure explains itself (what was awaited, last game state, screenshot, log tail). Agents find and use the harness from a project skill without reading source.

## Constraints

- Test mode only: normal play is unchanged unless test flags are given.
- Visible window shown without activation; no system input (SendInput, hooks, cursor moves); isolated `--save-root` copy (owner rules from G14).
- Acceleration allowed: waiting on game state and scaled game time. Not allowed: scene warps, hidden window.
- Memory writes into the game only for the opt-in `--god` cheat (health/ammo); everything else only reads state.
- Nativity: no device model or PPC interpretation; input enters through the existing `NativeInput::apply_test_command` / `apply_test_event` path.
- No new third-party dependencies (C++ and Python standard libraries; JSON handled with a small hand-written parser for the fixed protocol shapes on the C++ side).

## Architecture

```
python -m uctest run <scenario>          scripts/uctest/ (client, menus, scenarios, CLI)
        │  JSON lines over \\.\pipe\uc-test-<name>
        ▼
uc.exe --test-server <name> --save-root <copy> [--speed k]
   game/test_server.{h,cpp}  ── pipe thread: read commands / write events (mutex-protected queues)
   main loop, once per frame ── drains due commands → NativeInput; emits frame event
   runtime/clock.cpp         ── virtual clock (speed factor)
```

### 1. Test server (`game/test_server.{h,cpp}`)

- Enabled by `--test-server <name>`; requires `--save-root` and a Vulkan window (same rule as today's `--test-control`). One client connection; a second connection is refused.
- A pipe thread reads command lines and writes event lines; the main thread exchanges them through two mutex-protected queues. The pipe thread never touches game state.
- The main thread, once per presented or logical frame, applies every command whose target frame is due, then emits a `frame` event. Commands for a future frame wait in the queue.
- Replaces the file-polled `--test-control`. The old option stays until `train_route.py` is ported, then it is removed with that script.
- Client disconnect: the game releases all held inputs and keeps running until `--seconds` or the window closes (owner can inspect the state).

### 2. Protocol (JSON lines)

Commands carry an integer `id`; every command gets exactly one reply `{"id":n,"ok":true,...}` or `{"id":n,"error":"text"}`. Unknown fields are errors, not ignored.

| Command | Fields | Reply |
|---|---|---|
| `input` | `at_frame` (optional, default next frame), `hold_frames` (default 1), one of: `kpad:{x,y,buttons}`, `key:{code,down}`, `mouse:{button,down,x,y}`, `wheel:{delta}`, `motion:{x,y}` | `consumed_frame` once KPADRead consumed it |
| `capture` | — | `path` of the PNG after the GPU slot completes |
| `state` | — | full state snapshot (as `frame` plus raw probe words) |
| `speed` | `factor` 1..8 | `factor` applied |
| `quit` | — | process exits normally after the reply |

Events (no `id`):

- `frame`: `n` (game frame counter), `speed`, `context` (`inactive|menu|gameplay|qte`), `ui` (12 UI slot states), `pause`, `scene_mode`, `tutorial`, `player` (`valid`, `mode`, `state`; `health`/`ammo` added once their offsets are verified in an implementation task), `render_ms`, `presents`, `retraces`.
- `action`: `name`, `accepted`.
- `log`: diagnostic line (only lines the game already prints with a `[tag]`).
- `fatal`: crash report path and reason; `exit`: exit code.

Limits: command line ≤ 4 KiB, ≤ 256 queued commands, `hold_frames` ≤ 600, coordinates finite in [-1,1]; violations reply with an error and change nothing.

### 3. Virtual clock (`runtime/clock.cpp`)

- `now_ticks() = base + (real − base) × speed`; speed changes rebase so time stays monotonic.
- Every real-time wait derived from ticks (scheduler sleeps, alarms, VI period, AX block timer, frame limiter) divides by the speed, so the whole runtime advances consistently.
- With speed > 1, mixed audio is dropped instead of queued; only every speed-th frame is presented (render skip) while logic runs every frame.
- Subsystems verified at speed 2/4/8 during implementation: VI retrace callbacks, OS alarms, AX/audio startup wait (TD-048), movie playback, input sampling. A subsystem that breaks makes the client drop to speed 1 for that step; the limitation is recorded in tech debt.
- `frame.speed` lets the client refuse performance measurement at speed ≠ 1.

### 4. Python client (`scripts/uctest/`)

- `Game` context manager: creates `build/autotest/<timestamp>/` with a saves copy, launches `dist/uc.exe` visible-without-activation, connects to the pipe (bounded retry), runs a reader thread that keeps the latest `frame` event and a bounded history, and always terminates the process on exit (including Ctrl+C and client exceptions).
- Primitives: `wait(predicate, frames=…, seconds=…)`, `press(button, frames=2)`, `aim(x, y)`, `key(code, frames)`, `capture()`, `state()`, `speed(k)`.
- Steps: `skip_movies()` (press A while context is `inactive`/movie until it leaves), `menu.select(target)`, `wait_gameplay()`, `perform(action)` (waits for the matching `action` event), `measure_fps(seconds)` (speed 1, gameplay context only, drops samples with captures or context changes).
- Failure (timeout, game exit, fatal, protocol error) raises `StepFailed` carrying the awaited condition, last frame event, a fresh capture when possible, log tail and crash report path; the CLI writes them into the run directory.

### 5. Menus (`scripts/uctest/menus.py`)

- Screens identified by the set of active UI slots in the `frame` event; items by aim coordinates (existing calibration: screen x = 640+800·aimX, y = 360+600·aimY at 1280x720).
- `menu.select(target)`: verify the expected screen → aim → wait 2–20 frames for hover → press A for 2 frames → wait for a UI/screen change; on no change retry up to twice, then `StepFailed` with a capture.
- Implementation task: locate the hovered item index in RAM. If found, `select` checks it before pressing; if not, the screen-change check stays and the gap is recorded.

### 6. Scenarios and CLI

- Scenarios are Python functions in `scripts/uctest/scenarios.py`: `boot_to_title`, `train_gameplay`, `controls` (fire, reload, grenade, knife, pause accepted), `perf` (gameplay FPS at speed 1).
- `python -m uctest run <scenario> [--speed k] [--measure s] [--repeat n]` prints a short summary and writes `report.json` (result, step timings, artifacts) into the run directory; exit code 0 only on success.
- `train_route.py` is replaced by `train_gameplay`/`controls`; G14 and the script are marked superseded.

## Autoplay (added 2026-10-15, owner-approved)

### Game data

- `frame.enemies`: list of `{id, alive, visible, head:[x,y], body:[x,y], health?}` in aim coordinates [-1,1]. Points are computed in the game from the enemy's world head position × the scene's view and projection matrices (the ones used for the frame), so they match what is drawn.
- `frame.player` gains `health`, `ammo`, `magazine` once their offsets are verified.
- Reverse-engineering tasks: enemy manager/list and per-enemy head position (bone or offset from the model root); player health and ammo; Game Over/Continue and chapter-result screens identified by UI slots like other menus.

### Bot (`scripts/uctest/autoplay.py`, scenario `autoplay`)

- Loop every few game frames: gameplay → pick target (visible, alive, nearest to the crosshair weighted by distance to the player), aim at its head, fire; reload on an empty magazine or in a lull; grenade optional for groups. QTE → perform the requested action. Menus/tutorials/results → advance; Game Over → Continue.
- Stuck detection: no progress (chapter/screen/kill counter) for M seconds of game time, or N failures at the same place → stop with a report.
- All decisions use game frames, not wall time, so the bot works at speed 1..8; the target is re-checked on a fresh frame before firing.

### Logging and report

- `autoplay.jsonl` in the run directory: screen/chapter changes, shots, kills (enemy disappeared or health 0), deaths, QTEs, periodic captures, FPS.
- `report.json`: furthest point reached and stop reason (completed, stuck, crashed with crash report path).

### Cheat

- `--god` (requires `--test-server`): the server keeps player health and ammo at maximum every frame by writing their verified addresses; the report marks cheated runs.

## Error handling

- C++: malformed or out-of-range commands are rejected with a reply and no state change; pipe errors close the connection and release inputs; the server never throws into the game loop (errors become `log` events).
- Client: every wait has a frame budget and a real-time watchdog; game exit or `fatal` aborts the step immediately; the process is killed on any exit path.

## Testing

- C++ unit tests: protocol parser (malformed JSON, unknown fields, overflow, limits), frame-due command queue, virtual clock monotonicity and wait scaling, `frame` event serialization.
- Python tests: client against a fake pipe server (timeouts, disconnect, menu retry, measurement rejection at speed > 1).
- Autoplay: unit tests of target selection and the state machine on fake frames; live train chapter at speed 1 and 4, with and without `--god`, reporting how far it got.
- Live: `boot_to_title`, `train_gameplay` ×10 consecutive successes with timing versus `train_route.py`, `controls`, `perf`; speed 1 and 4.

## Documentation

- Project skill `.claude/skills/uc-test/SKILL.md`: when to use, ready commands, reading `report.json`, common failures.
- `docs/testing/test-server.md`: protocol reference.
- A short root `AGENTS.md` (the repo has none; Codex reads it) pointing to the skill and the protocol reference; G14 and `train_route.py` marked superseded.

## Out of scope

LLM-driven free play, scene warps, hidden/headless runs, Dolphin-side automation changes, image comparison beyond invoking existing tools.
