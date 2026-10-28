---
name: uc-test
description: Run Umbrella Chronicles port scenarios without the owner — reach title/train gameplay, test controls, measure FPS, capture frames. Use for any live check of uc.exe behaviour, regression or performance.
---
# Umbrella Chronicles test harness
Build first (`build.bat --no-install` stages `dist`). Then from `scripts/`:
- `python -m uctest list` — scenarios.
- `python -m uctest run train_gameplay [--speed 8]` — reach train gameplay, capture; report in `build/autotest/<run>/report.json`.
- `python -m uctest run controls` — fire/reload/grenade/knife/pause must be accepted.
- `python -m uctest run gamepad` — the gamepad through the host-event path: analog right stick (Nunchuk), left stick aim, L3 recenter, buttons, trigger, gyro swing. `run gamepad_prompts --model xbox|playstation|nintendo|generic` captures the menu prompts in that controller's look.
- `python -m uctest run perf --measure 15` — menus at 8x, FPS measured at speed 1 in gameplay.
- `python -m uctest run fight [--measure 15] [--mark]` — the bot plays at 8x and the game drops to speed 1 at the saved slowdown point (`scripts/uctest/marks/fight.json`: the owner presses F5 once when the fight is on screen, a global key read, no focus change; `--mark` or F5 later replaces the point), then an idle god-mode player and FPS measured. With `UC_CPU_SAMPLE=guest UC_CPU_SAMPLE_FLAG=<file>` the CPU sampler starts at the slowdown. Tutorials are always skipped here.
- Comparing two builds (the reliable way): `python -m uctest demo record fight1` once (the live bot fights in deterministic mode while the game records every input the guest reads, build/demos/fight1.demo), then `python -m uctest bench <exe-A> <exe-B> --demo fight1 --runs 4` (each run replays the demo without bot or harness: identical guest work, 787039 draws every run, run-to-run sd 1-3%, the verdict is a paired 95% CI plus best-vs-best). `python -m uctest demo check fight1` proves replay == recording (hash of guest memory per retrace); re-record after a change that alters what the guest can see. `python scripts/det_check.py` checks a plain run from launch for determinism. Without `--demo`, `uctest bench` plays the fight live (noise 7-13%, only for large effects). Deterministic runs use `--deterministic` (virtual clock, events from other threads only while the guest is idle); see memory note deterministic-mode.
- `python -m uctest run autoplay [--god] [--speed 8] [--max-minutes 10]` — bot plays the train chapter (aims at enemy heads read from game memory, skips tutorials, continues after death); `--god` keeps health/ammo full so it plays on until the next crash. A `crashed` report names the crash report file.
- `--skip-tutorials` (any scenario) — tutorial pages never open; `python -m uctest run no_tutorials --skip-tutorials` checks it.
- `python -m uctest inspect <crash-*.txt> threads|stack|mem|f32|nan|posmtx|gx` reads the crash bundle (guest memory, registers) offline.
- `--save-as NAME` keeps a run's saves as a checkpoint; `--from NAME|<saves dir> --chapter N` starts there (chapter 2 gameplay in ~10 s).
- `--repeat N` for reliability. Speed 8 reaches gameplay in ~4 s, speed 1 in ~18 s.
Reading results: exit 0 = all passed. Autoplay: `autoplay.jsonl` (decisions) and `report.json` → `result.autoplay.reason` (`stuck`, `time_limit`, `crashed`, `harness_error`). On failure open `failure.json` in the run directory (awaited condition, last frame, capture, log tail, crash report); `report.json` has step timings and `counters.press_retries`.
Writing a scenario: `from uctest.client import Game; from uctest import steps` — use `g.wait(predicate)` on the `frame` state and `g.advance(ticks)` for game time, never `sleep`; menus via `steps.select("<target>")`; actions via `steps.perform`. Add screens/targets in `scripts/uctest/menus.py`.
Rules: the game runs in a visible 1280x720 window, saves are an isolated copy (automatic), no system input, performance only at speed 1, look at captures yourself.
Protocol, frame fields, `peek`: `docs/testing/test-server.md`.
