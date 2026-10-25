# Test server protocol

`uc.exe --test-server <name> --save-root <isolated copy> [--capture-dir <dir>] [--speed 1..32] [--width 1280 --height 720] [--skip-intro] [--god] [--skip-tutorials]`
opens two single-client named pipes (JSON lines, UTF-8, `\n`):

| Pipe | Direction |
|---|---|
| `\\.\pipe\uc-test-<name>-cmd` | client → game |
| `\\.\pipe\uc-test-<name>-evt` | game → client |

`<name>` is `[A-Za-z0-9_-]{1,32}`. The window is visible but never activated; no system input is sent. The harness always passes `--no-vulkan-layers` (sets `VK_LOADER_LAYERS_DISABLE=~implicit~`): implicit layers such as OBS capture, RTSS, ReShade and the Steam overlay cost ~3 FPS and add 50+ ms hitches, so tests measure the game alone. Normal play keeps the layers on. Use `python -m uctest` instead of speaking the protocol by hand (see `.claude/skills/uc-test/SKILL.md`).

All `uctest` game launches (live scenarios, bench, demo recording/replay/check/profile) pin guest threads with `--guest-cpu 26`. Other CPU arguments are rejected; the old local `build/bench-machine.json` no longer overrides placement. This harness configuration requires a machine where logical CPU 26 is available; normal launcher calibration is independent.

## Commands

Every command carries an integer `id` (≥ 1, unique among open commands) and gets exactly one reply `{"id":n,"ok":true,...}` or `{"id":n,"error":"text"}`. Unknown fields are errors. A line is ≤ 4096 bytes, at most 256 commands are open.

| `cmd` | Fields | Reply fields |
|---|---|---|
| `input` | `at_frame` (default: next frame), `hold_frames` 1..600 (default 1, kpad/key/mouse only), exactly one of `kpad:{x,y,buttons}` (x,y in [-1,1]; buttons ⊆ 0x1C10 = A 0x800, B 0x400, plus 0x1000, minus 0x10), `key:{code 1..511,down}`, `mouse:{button 1..255,down}`, `wheel:{delta ±256}`, `motion:{x,y}` | `consumed_frame` once KPADRead consumed it |
| `capture` | — | `path` of the PNG (after the GPU slot completes) |
| `state` | — | `state` = the latest `frame` event |
| `speed` | `factor` 1..32 | `factor` |
| `draws` | — | `draws`: one `{pass,callsite,vertices,tex:[w,h,id],bbox:[x0,y0,x1,y1]}` per draw of the packets rendered since the request (bbox in NDC, y down), `projection`: the scene's 4x4 perspective projection row-major (`[]` if none) |
| `peek` | `addr` (MEM1 `0x80000000`+24 MB or MEM2 `0x90000000`+64 MB), `size` 1..262144 | `hex` (uppercase) — read-only diagnostics / reverse engineering |
| `host_key` | `code`: SDL scancode 1..511 — the port's own keys: 73 Insert (settings menu), 19 P (pause), 41 Escape (closes the menu) | — (applied in the main loop's next iteration; frame fields `menu_open`, `paused`) |
| `menu_click` | `x`, `y`: window pixels (0..16384) | — (a left click on the in-game menu: the pointer moves there, then presses and releases; ignored while the menu is closed) |
| `options` | `graphics`: object of `config.json` graphics keys (`render_scale`, `upscaler`, `upscale_mode`, `frame_generation`, `fps_limit`, `bloom`, `prompt_style`, ...) | — merged over the saved settings, applied live and saved, as the in-game menu does; frame fields `upscaler`, `frame_generation` show what the renderer runs. The harness passes `--config <run dir>/config.json` (a copy of the dist config), so saves never touch it |
| `quit` | — | process exits normally after the reply |

A `kpad` press holds its buttons for `hold_frames` and then releases them keeping the aim; key/mouse `down:true` releases after `hold_frames`. A later kpad command owns the pad state, an earlier command's release is then skipped. When the client disconnects the game releases every held input and keeps running until `--seconds`.

## Events (no `id`)

- `frame` (every KPAD sample, droppable under backpressure): `n` frame counter, `speed`, `context` (`inactive|menu|gameplay|qte`), `ui` (12 UI manager slot states, `-1` inactive), `menu` (4 menu page-alive words), `ui_valid`, `pause` (2 = paused), `scene_mode`, `tutorial`, `tutorial_loaded`, `player{valid,address,mode,state,magazine,life,damage,scene}` (`life` HUD gauge 0..1, `damage` 0 = unhurt .. 50 = dead), `view` (world→view 3x4 row-major or `null`), `enemies` (`[{id,cls,root:[x,y,z],head,hp}]`: `cls` actor class pointer, `head` false once the head is shot off, `hp` zombie health 10 per pistol hit, <= 0 is dead although the actor stays until the wave ends; world units of 10 cm, y up; actors outside the 200-entry table are found through the actor list), `render_ms`, `presents`, `retraces`, `dropped` (frame events dropped).
- `action`: `name` (`Reload|Grenade|Knife|QTE`), `accepted`, `frame`.
- `exit`: `code`.
Crash information is not sent on the pipe: the client reads the tail of `run.log` and the newest `debug/crash-*.txt` itself when the process dies.

## Codex Windows sandbox

On this machine, launch the harness outside the Codex process sandbox when testing GPU behavior. The isolated process environment makes NVIDIA repeat expensive pipeline creation: the same executable and byte-identical application cache warmed 346 pipelines in 8.5 seconds inside the sandbox, versus 0.47 and 0.52 seconds in consecutive ordinary launches (2026-10-21). Keep all harness window, mute, input and copied-save protections. Shader-cache deletion is unnecessary.

For CTest, use a temporary directory inside `build`. Tests that exercise atomic file replacement can still receive `Access denied` inside the sandbox; rerun those tests outside it before treating the failure as a product regression.

## Time

`n` counts KPAD samples. `retraces` counts VI retraces = game time at 60 ticks/s at any speed. Menu animations, loading and tutorials follow game time, so waits that depend on them use `retraces` (`Game.advance(ticks)`). With `--speed k` the whole runtime clock is scaled by k (verified at 1/2/4/8: retraces/s = 60·k). Above 4 the KPAD/present rate is capped near 160–190 Hz (rendering is not skipped), so game logic frames per game tick fall below 1.

`frame.game_seconds` is the accumulated game timestep from `game_pacing_stats`,
in seconds. Use its change over wall time to distinguish slow simulation from low
render FPS. In network mode an input reply waits until the delayed canonical input
is applied; capture alone does not acknowledge consumption.

Speeds 16 and 32 are accepted (2026-10-15, `--skip-tutorials`, train chapter 1, 6 s wall in gameplay, render scale 2x): retraces/s 893 at x16 and 1714 at x32 (ideal 960/1920), but KPAD/present frames/s stay at 88 (x8), 83 (x16), 79 (x32), against 30 at x1. The game logic therefore runs at most ~2.9x faster than real time whatever the factor; the ceiling is the guest CPU cost per frame, not the clock (`--render-scale 0.5` and `1` give the same 85-89 frames/s). Above x8 the extra clock only gives each logic frame a larger time step and costs more menu retries; use x8 unless the frame cost drops (docs/spikes/F3-guest-frame-cost.md, F4-gx-offload-estimate.md).

## Menu detection (state, not timers)

`scripts/uctest/menus.py` identifies screens from the `ui` slots and four guest words (`InputProbeSnapshot::menu_pages`: `804BB408` chapter page alive, `804BF4C0` character, `804C0574` loadout, `804D0994` start dialog). A page object is created 45–120 ticks after the press that opens it; presses before that are dropped. `steps.select` aims, presses A for 3 samples and repeats every 30 ticks until the next screen shows. Moving backwards is not modelled.

## Enemies and autoplay

Memory layout: `docs/spikes/G22-autoplay-memory.md`. `scripts/uctest/vision.py` projects `enemies` with `view` and the scene `projection` (`draws` reply) to aim coordinates. `python -m uctest run autoplay [--god] [--speed k] [--max-minutes m]` runs `scripts/uctest/autoplay.py`: it acts once per game frame (also during grabs, context `qte`), dismisses tutorial pages (state `tutorial_loaded` and `tutorial` not in {0, 0x100000}), fires at the nearest living enemy head of the zombie class (`hp` > 0; 30 shots without a kill -> skip for 600 ticks), fires at the screen centre whenever there is no enemy it can aim at (bosses, leeches, crawlers it cannot locate), reloads on an empty magazine, continues after Game Over, ends with `chapter_done` when the chapter-select page appears after a finished chapter (save it with `--save-as`), and otherwise stops with `stuck` (no kill or screen change for 300 game seconds), `time_limit`, `crashed` (game exited; crash report path in the report) or `harness_error`. Trigger cycle: 2 frames down, 1 up (a 1-frame press fires 12 of 26 times). At speed 8 the game runs ~5 logic frames per game second, so the fire rate per game second is bounded by TD-050. Outputs: `autoplay.jsonl` (`shot`, `centre`, `kill`, `reload`, `tutorial`, `menu`, `unknown_menu`, `death`, `ignore`, `capture` with the enemies the bot saw, `stop`, `report`) and `report.json`.

## Window resize

`python -m uctest run resize` changes the window size every ~10 ms through `SetWindowPos` (no activation, no input) and fails when game time runs below 80 % or the frame events below 70 % of idle while it happens (originally 22 ticks/s and 5 frames/s of 60/30; now 59 and 26). While Windows runs its modal drag/resize loop the main loop is blocked inside `SDL_PollEvent`; an SDL event watch runs the same service step (harness commands, input, rendering) from `SDL_EVENT_WINDOW_EXPOSED`, and swapchain rebuilds are spaced 60 ms apart (`renderer/rebuild_gate.h`) with the old swapchain presenting (or skipping the present) in between. A rebuild waits only for the slots' GPU work, keeps buffers, textures and the EFB, passes the old swapchain as `oldSwapchain` and frees it when its presentations finished (7 ms instead of 45 ms). The event watch keeps rendering while packets wait (a guest frame is ~2.4 packets, the modal loop's timer ticks 64-100 times a second) and `timeBeginPeriod(1)` makes that timer fine-grained.

## Checkpoints (start late without playing early)

The game saves progress into the isolated saves copy. `--save-as NAME` keeps a finished run's saves as `build/autotest/checkpoints/NAME`; `--from NAME|<saves dir>` starts from them (copied, never modified) and `--chapter N` goes straight to chapter N (it must be unlocked there). Example: `python -m uctest run autoplay --god --speed 8 --save-as after-ch1`, later `python -m uctest run autoplay --god --speed 8 --from after-ch1 --chapter 2` reaches chapter 2 gameplay in about 10 s. This is a progress checkpoint; a true mid-chapter savestate is not possible (host threads, GPU and HLE state live outside guest memory).

`--god` writes damage 0, magazine ≥ 99, grenades 9 every KPAD sample (cheat; reports carry `"god": true`).

`--skip-tutorials` (uctest: `run <scenario> --skip-tutorials`) turns the original tutorial opener `8024F280(object=804B5CC8+234C, id)` into a no-op: at its entry the gate byte `object+4` is zeroed (the opener then returns 0 by itself) and the state `object+0x48` is set to the closed value `00100000`, which the scenario script at `801092A0` polls through `8024F558`; the gate byte is restored on exit. No page opens, `tutorial_loaded` never pairs with a state other than `0`/`00100000`. `python -m uctest run no_tutorials --skip-tutorials` watches 20 s of game time from the mission start and fails on any open page (without the flag it fails at the first one).

## Crash bundle

Every fatal error writes `dist/debug/crash-<time>-<pid>.txt` (host stack with guest function names) and, next to it, the folder `crash-<time>-<pid>.dump` with the machine state at the moment of the crash (about 88 MB each; delete old ones by hand): `mem1.bin` (guest 0x80000000, 24 MB), `mem2.bin` (0x90000000, 64 MB), `threads.txt` (every live guest thread: state, wait key, r0-r31, lr/ctr/cr/msr, f0-f31 as raw bits; `current 1` marks the thread that crashed), `gx.txt` (bound vertex/matrix arrays, 12 = position matrices, 13 = normal matrices) and `functions.txt` (entry address and symbol of every translated function). The report ends with a `bundle:` line; a failed autoplay/scenario report names the `.txt`.

`python -m uctest inspect <crash-*.txt|*.dump> threads | stack [thread id] | mem ADDR [SIZE] | f32 ADDR [COUNT] | nan ADDR SIZE | posmtx INDEX | gx` reads it back offline, any number of times, without the game: the back chain from r1 with function names, memory and float dumps, non-finite floats in a range, and the position matrix an indexed load (`GXLoadPosMtxIndx`) reads (`posmtx INDEX` uses array 12 of `gx.txt`; INDEX is r3 of the crashed thread). It is a post-mortem view, not a savestate: the game cannot resume from it (guest threads live on host stacks).

Open (2026-10-15): `autoplay --god --speed 8 --max-minutes 2 --skip-tutorials` plays the chapter with no tutorial event at all (97–99 kills) but did not finish in 3 of 3 runs, while the same command without the flag reached `chapter_done` in 3 of 3: twice the frame stream stopped at the results screen (`timeout waiting for next game frame`, the game kept running and rendering), once the guest crashed in a J3D draw (`GX error 0 at 8028882C: expected 12, received 0`, tick 38401). Cause not found; use the flag for menu/gameplay scenarios (`no_tutorials`, `train_gameplay`, `perf`) until it is.

## In-game menu and pause checks

The in-game settings scenario is `python -m uctest run overlay`: Insert opens the menu over gameplay, graphics changes are applied and saved in the run's isolated config, render scale survives every upscaler switch, and Escape closes it. It covers bloom on/off, both prompt styles, FPS limits, DLSS/FSR quality, RCAS enable/strength/disable, repeated DLSS/FSR off/on, generator toggles and generator-family changes. These upscaler and generator changes preserve the device. `run.log` records host update timings.

Harness scenarios and deterministic replays default to a 1280x720 window, overriding a saved fullscreen setting. Keep this window size during normal tests; use `--game-arg=--render-scale --game-arg=2` for a higher internal render resolution. Fullscreen or a different window size requires an explicit game argument.

`ctest --test-dir build -R '^test_settings_menu$' --output-on-failure` drives real ImGui input at 1440p: 120 frames must preserve style metrics, the window remains 746×1200 with the existing 29-pixel body font and checkbox squares, controls have 75% of the previous height, both tabs remain visible, every main combo opens, a scale choice applies, RCAS toggles and slider changes persist to an isolated config, and dragging moves the menu. The optional GPU variant, `build/tests/test_settings_menu.exe --gpu dist/data/shaders build/menu-ui-final`, repeats the interactions through the Vulkan renderer and saves `main.png`, `combo.png`, `rcas.png`, `controls.png` and `dragged.png` for visual checks. It uses a hidden test window and synthetic ImGui events; no system input or game saves.

`python -m uctest run pause` checks the title screen, gameplay, opening the menu over a paused scene, ignoring P while the menu is open, and resuming. Repeat with `--game-arg=--fps-limit --game-arg=30` or `60` to check capped modes. Enemy positions, HP and player health must match exactly; camera matrix entries allow absolute float roundoff of `1e-5`; captures allow at most 0.05% changed pixels for the known sub-pixel bone movement. Cutscenes, QTE and audio playback continuity remain unverified by this scenario; the guest audio timeline can advance while the output stream is paused.

The demo benchmark currently needs Python 3.12 or newer; on this machine use `py -3.13 -m uctest bench ...` because the bare `python` command selects 3.10.

## Known gaps

- Hover item and button rectangles of menu pages are not read from RAM (only the scenario page showed a hover word at `804B96BC`). Targets are measured aim coordinates (`screen x = 640 + 800·aimX`, `y = 360 + 600·aimY` at 1280x720).
- "Accepts input" per page is unknown; the harness repeats presses instead (counted as `press_retries` in `report.json`).
- Rendering is not skipped at speed > 1; logic is capped near 160–190 KPAD samples/s above speed 4.
- Performance may only be measured at speed 1.
- Enemy aiming works for the zombie class only (`0x803F2B60`); other classes (boss, leeches, other chapters) are not aimed at and the bot fires at the screen centre. Dead = `hp` <= 0. See G22.
- `--test-control` (file polling) stays until `scripts/train_route.py` is removed by the owner.
