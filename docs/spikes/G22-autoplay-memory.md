# G22: game memory used by the test harness and autoplay (RBUE08, NTSC-U)

Found 2026-10-15 with the test server `peek` command (paused full-MEM1 dumps and per-tick object snapshots), cross-checked against the Dolphin Gecko codes in `memory_cheat_codes.txt`. All addresses are guest addresses. Confidence: H = confirmed live, M = consistent evidence, L = guess.

| What | Address | Evidence | Conf. |
|---|---|---|---|
| Actor list | `804421C4`: 200 × (id u32, object pointer u32) covers ids < 200; every actor is also in a doubly linked list (object `+0` id, `+4` previous id, `+8` next node = next object + 8, `+C` previous node), the probe walks it from player 1 | H (ids 600+ roof zombies found by scanning MEM1 for the class word) |
| Actor table (old row) | `804421C4`: 200 × (id u32, object pointer u32) | ids 1..163 at train start; pointers match a doubly linked actor list (id +0, previous id +4, next node +8, previous node +C) | H |
| Actor class | object `+10` | `803F2388` players (ids 2, 3), `803F1A40`/`803F7EA0` ~157 pooled objects, `803F2B60` the zombies | H (train), L (other chapters) |
| Enemy world position | object `+F48` (x, y, z floats; units of 10 cm, y up) | changes smoothly, projects onto zombie feet in captures | H |
| Zombie health | object `+3A4` float (20 / 10 / 30 at spawn, −10 per pistol hit, ≤ 0 dead; the actor stays in the list until the wave ends, so a missing hp flag made the bot shoot corpses) | series of every zombie object during wave 1: monotone, hits at the bot's shot ticks | H |
| Head intact | object `+1058` float (1.0 intact, 0.1 after a head shot) | flips at the first head hit, aim point switches to the torso | M |
| Camera | `8044B4E0`: world → view 3x4 row-major (rows orthonormal) | updated every frame, projecting enemy positions with it and the scene projection from the GX draws lands on the heads (overlay) | H |
| Scene projection | `draws` reply `projection` (first perspective projection of the frame) | `[1.4706,0,0,0, 0,-2.3764,0,0, 0,0,-1,-0.01, 0,0,-1,0]` | H |
| Aim ↔ NDC | aim x = ndc x · 0.8, aim y = ndc y · 0.6 (ndc y down) | 1280x720 calibration `px = 640 + 800 aim x`, `py = 360 + 600 aim y` | H |
| Magazine | player `+4D4` (probe `weapon_words[3]`) | 15 → 14 → 13 per pistol shot, back to 15 on reload; Gecko "No reload" writes `E4C5B6` = player + 4D6 | H |
| Damage (health) | scene `+6C00` float: 0 unhurt, 10 per hit, 50 dead; P2 at `+6C14` | 0,10,20,30,40,50 against HUD life 1.0…0.0; Gecko "Infinite Health P1 alternative" zeroes `80BE55A0` = scene + 6C00 | H |
| HUD life gauge | `804B7960` float 1.0 … 0.0 in steps of 0.2 | display copy of 1 − damage / 50 | H |
| Grenades | `8056F338` word | Gecko "Infinite Grenades" writes 9; the HUD shows 9 after the cheat | H |
| Game Over screen | UI slot 4 = 7 (`ui` of the frame event) | captured "You died - Continue? Yes/No" | H |
| Menu page words | `804BB408`, `804BF4C0`, `804C0574`, `804D0994` | set when the chapter / character / loadout / start-dialog page object exists | H |

## `--god`

`uc.exe --test-server <name> --god` (guest thread, once per KPAD sample) writes damage = 0, magazine ≥ 99 and grenades = 9. Cheated runs are marked `"god": true` in `report.json` and `autoplay.jsonl`.

## Findings of autoplay runs

- With `--god` the train chapter advances in waves of 4–7 zombies (kills are logged per wave when the actors leave the table). Run at speed 8 on 2026-10-15 it reached 19 enemies and ~4 min of game time before the game crashed: `fatal: uncaught C++ exception in guest thread 1: unsupported native mesh triangle primitive` in the model draw function `80366E14` (crash report in `dist/debug/crash-20261015-134352-39216.txt`, TD-052).
- A target that cannot be hit (behind a wall or in the next car) is skipped after 30 shots for 600 ticks.

## Gaps

- Zombie class `803F2B60` is the only class with a known layout (position `+F48`, hp `+3A4`, head `+1058`). Classes seen at the Leech Man (`80408534`, `803FBA30`, `80411484`) have no position at `+F48`; the boss and leeches are not located, the bot fires at the screen centre. Other chapters need their classes classified.
- Chapter result screen and Game Over → Continue target (`game_over_continue`, aim 0.22/0.325) are not verified: no run reached them (god mode never dies, the chapter did not finish before the crash).
- Player 2 and ammo reserves other than grenades are not read.
