---
name: uc-test
description: Run Umbrella Chronicles port scenarios without the owner — reach title/train gameplay, test controls, measure FPS, capture frames. Use for any live check of uc.exe behaviour, regression or performance.
---
# Umbrella Chronicles test harness
Build first (`build.bat --no-install` stages `dist`). Then from `scripts/`:
- `python -m uctest list` — scenarios.
- `python -m uctest run train_gameplay [--speed 8]` — reach train gameplay, capture; report in `build/autotest/<run>/report.json`.
- `python -m uctest run controls` — fire/reload/grenade/knife/pause must be accepted.
- `python -m uctest run perf --measure 15` — menus at 8x, FPS measured at speed 1 in gameplay.
- `python -m uctest run autoplay [--god] [--speed 8] [--max-minutes 10]` — bot plays the train chapter (aims at enemy heads read from game memory, skips tutorials, continues after death); `--god` keeps health/ammo full so it plays on until the next crash. A `crashed` report names the crash report file.
- `--save-as NAME` keeps a run's saves as a checkpoint; `--from NAME|<saves dir> --chapter N` starts there (chapter 2 gameplay in ~10 s).
- `--repeat N` for reliability. Speed 8 reaches gameplay in ~4 s, speed 1 in ~18 s.
Reading results: exit 0 = all passed. Autoplay: `autoplay.jsonl` (decisions) and `report.json` → `result.autoplay.reason` (`stuck`, `time_limit`, `crashed`, `harness_error`). On failure open `failure.json` in the run directory (awaited condition, last frame, capture, log tail, crash report); `report.json` has step timings and `counters.press_retries`.
Writing a scenario: `from uctest.client import Game; from uctest import steps` — use `g.wait(predicate)` on the `frame` state and `g.advance(ticks)` for game time, never `sleep`; menus via `steps.select("<target>")`; actions via `steps.perform`. Add screens/targets in `scripts/uctest/menus.py`.
Rules: the game runs in a visible 1280x720 window, saves are an isolated copy (automatic), no system input, performance only at speed 1, look at captures yourself.
Protocol, frame fields, `peek`: `docs/testing/test-server.md`.
