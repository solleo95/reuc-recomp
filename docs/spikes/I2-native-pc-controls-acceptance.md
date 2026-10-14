# I2 — Native PC controls acceptance

2026-10-14, RBUE08 user image. Checkpoint d8835e5 precedes this work. Stage1 motion-QTE remains under live acceptance. Stage2 config/launcher now has working native UI; port-owned prompts remain mandatory stage3.

## Verified

| Behavior | Evidence |
|---|---|
| LMB menus/Interact, RMB Fire | v2 host-event route; original magazine15→12 in1791926726677724300 |
| R Reload | original refill12→15/state2; accepts in1791932237986416400 and1791932770890224300 |
| S / wheel selection | original weapon0→2/ammo50; ordered-step/focus/held-key fixtures |
| G and Interact+Fire | two actual grenade accepts in1791932237986416400, inventory3→1 in its capture |
| Held V and directional gesture | seven accepts in1791932237986416400, eight in1791932770890224300; original cooldown and both-order direction fixtures |
| Esc Pause / Back | original Minus entry, generic Back exit; live pause screenshot and Menu→Gameplay transitions |
| Tutorial context | actual tutorial.csv/TRL object busy getter; loaded state00100000 is terminal; live Menu/Gameplay transitions align with tutorial states |
| Focus/config/actor cancellation | generation, cleared/remapped combos, yielding-body/new-actor and original selection-parent fixtures |
| Bare mouse motion-QTE | timestamp/reversal/cadence/window/target/reset/count fixtures; four verified original counter callers; live encounter pending |
| Effect rendering needed by controls | native A8 view, line topology, RGB565 pause copy; Vulkan validation/pixel/recorded/recycle fixtures |
| First-carriage progression | 1791964017664063800: original shooting/reloads move from restaurant car into narrow corridor; healthy actor/HUD and live capture59701 below |
| Saved cleared/rebound controls | 1791966034773076400: Grenade/Knife=[] leaves grenades3 and only Reload accepts.1791966300075745700: Fire onF/I, Grenade onH, Knife onB; Reload/Grenade/eight Knife accepts through the same NativeInput API |
| Launcher editing/save/launch | build/launcher-smoke/run-7 real widget click/capture/save/stale/defaults; запуск-8 actual Launch widget starts visible/audible/skip-intro isolated game with Unicode config path and normal exit0 |

Diagnostics are visible, audible, skip intro and use isolated save copies. Input is published inside the game's own NativeInput path; no OS mouse/keyboard injection.

## Live captures

- frame-1791930132202273-11904-10136.png: actual pause menu over healthy train scene.
- frame-1791932292598553-42084-25180.png: train/HUD, SMG and one grenade remaining, no tutorial.
- frame-1791932816399074-50336-18992.png: grenade flash/train/HUD; effect image does not establish steady scene parity.
- frame-1791964135566801-36732-59701.png: corridor beyond the restaurant carriage, remaining health, enemies and subtitle «Поезд поехал.»; original progression, no game-state writes by the route.
- build/launcher-smoke/run-7/{cleared,saved,error,defaults}.bmp: actual native UI states, Cyrillic font and recorded save/error behavior.

Stage2 codec tests cover duplicate properties, unknown/invalid input schema, defaults/explicit empty roundtrip, writer exclusion, stale snapshots, Unicode paths and a real Windows replacement failure while a reader holds the original file. Original bytes remain intact and saving succeeds once that reader releases the file. Scoped Luna reviews of codec and editor/launcher: no issues. Whole-plan review remains pending until live-QTE acceptance is complete.

Live multi-source/focus route1791968126465971700 exposed v2 rejecting physical key-up while unfocused and treating that as a fatal protocol error. RED fixture→minimal release exception in apply_test_event→GREEN input/game_input/host_input. Down/motion/wheel while unfocused remain rejected; release clears the resolver's blocked physical source.1791968355975530100 repeats two Fire keys, releases one without a game Fire-release edge, loses/restores focus, then successfully accepts Reload/Grenade/eight Knife. Controlled events36..40 acknowledged. Updated executable staged and copied/E after targeted checks; no user save/config writes.

Full build.bat --no-install:76/76CTest, dist installed. Copied dist to S:/Games/Umbrella Chronicles with robocopy/E (code1 means files copied). No config.json/saves in dist; user files preserved. New executable uc_launcher.exe contains current graphics/intro controls and input editing; future DLSS/FSR/FG and port-owned prompts are not implemented here.

1791932770890224300 presentation samples31,30,30,30,30,30; context at measurement end Gameplay. No performance gain claimed from this short segment. Previous6/21/30 samples included effect/capture costs; modal/death intervals are not battle FPS. Original30FPS cadence remains default.

## Outstanding acceptance

- Corridor progression is visually verified; add a proven read-only location predicate before calling it an automated progression assertion.
- Exercise a live motion-QTE, its successful/failed windows and different mouse/DPI settings. Initial gesture defaults24raw units/leg and600units/sec are provisional calibration.
- Persisted input config, live cleared/rebound profiles and launcher UI/capture are verified. Broader live multi-source focus/cancellation and further encounters remain to check.
- Finish final whole-plan Astra review after all required acceptance; repeat deployment only after new changes/checks. No additional commit without owner request.

Neutral SDK motion metadata remains TD-041; regional/language profiles, exact GX effect rounding/line widths and other render parity remain separate debt. Game assets and user saves are unchanged.
