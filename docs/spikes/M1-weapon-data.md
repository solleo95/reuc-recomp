# M1: weapon data and player structures for the mod API (RBUE08, NTSC-U)

Found 2026-10-22 for the mod platform (`docs/superpowers/specs/2026-10-22-mod-platform-design.md`). Evidence: the translated code (`generated/.../game_007.cpp`, `game_046.cpp`), test-server `peek` reads of a live train chapter, and live experiments with a Lua mod through the harness (`Game(mods=True)`). All addresses are guest addresses.

## Player and scene (confirmed live)

| What | Where | Notes |
|---|---|---|
| Player pointers | `806C3AD8`: P1, `806C3ADC`: P2 | SDA slot `r13 - 27464` (`r13 = 806CA620`); both players exist in the train chapter |
| Scene pointer | `806C3B78` (`r13 - 27304`) | train: `80B9E9A0` |
| Damage meter | scene `+6C00` (P1), `+6C14` (P2), float 0 (unhurt) to 50 (dead) | matches `--god` |
| Magazine | player `+4D4`, u32 | 15 at the start, 14 after a shot |
| Weapon object | player `+48` | `+480` weapon id (player `+4C8`), `+484` inventory key (0: infinite reserve), `+48C` magazine, `+490` state (2 while reloading; player `+4D8`) |
| Grenades | `8056F338`, u32 | one stock for both players (Gecko "Infinite Grenades" writes 9) |
| Camera | `8044B4E0`, 3x4 floats | |
| Actor table | `804421C4`, 200 x (id, pointer) | actors with ids 200+ only in the linked list from the player (`+8` = next object + 8) |

## The weapon table

`8006C8C8(id)` clamps the id to 0..19 and returns `8046A86C + id * 168`: 20 records of 42 words. The capacity getter `801D431C` reads record `+0x14` (or `+0x44` for another weapon class); the weapon update `8006C26C` reads `+0x04`, `+0x10`, `+0x30`, `+0x5C`; the reload starter `8006D980` reads `+0x38` and `+0x58`.

| Id | capacity `+14` | damage `+24` | rate `+0C` | mode `+10` | reload `+38` |
|---|---|---|---|---|---|
| 0 | 15 | 10 | 1 | 1 | 1.0 | handgun (the player's first weapon) |
| 1 | 6 | 9 (x12 pellets, `+18`) | 1 | 1 | 2.3 | shotgun |
| 2 | 50 | 4 | 13 | 0 | 0.5 | submachine gun |
| 3 | 30 | 5 | 15 | 3 | 1.0 | |
| 4 | 4 | 180 | 1 | 1 | 2.7 | |
| 5 | 1 | 100 | 1 | 1 | 1.0 | |
| 6 | 0 | 12 | 0 | 0 | 10.0 | |
| 7 | 4 | 120 | 1 | 1 | 2.3 | |
| 8 | 6 | 100 | 1 | 1 | 1.7 | |
| 9 | 6 | 7 (x10) | 1 | 1 | 2.0 | |
| 10 | 50 | 7 | 15 | 0 | 0.4 | |
| 11 | 6 | 80 | 1 | 1 | 2.0 | |
| 12 | 8 | 60 | 1 | 1 | 1.2 | |
| 13 | 3 | 120 | 1 | 1 | 2.3 | |
| 14 | 8 | 8 (x14) | 1 | 1 | 1.7 | |
| 15 | 40 | 8 | 10 | 0 | 0.7 | |
| 16 | 1 | 180 | 1 | 1 | 2.3 | |
| 17 | 3 | 300 | 1 | 1 | 2.0 | |
| 18 | 1 | 350 | 1 | 1 | 2.5 | |
| 19 | 15 | 10 | 1 | 1 | 1.0 | handgun variant |

Names other than 0 to 2 are not established; the API addresses records by id.

## Experiments (handgun, train chapter, harness speed 8, tutorials skipped)

| Change | Result |
|---|---|
| none | reload took 56 ticks, a held trigger (90 frames) fired once |
| `capacity = 30`, `reload_time = 0.2` | reload took 8 ticks and refilled 30 rounds |
| `fire_mode = auto` (`+10 = 0`), rate 10 | held trigger: 4 shots |
| same with rate 30 (`+0C`) | 4 shots: the rate field does not change the cadence |
| `+2C = 0.5`, `+30 = 0.02` written every step in auto mode | 4 shots: neither is the cadence either |

Findings that shaped the API:

- The game fills the table again after a mod's `on_load` (a first run, with a one-time write, showed the original values): the API keeps the values a mod set and writes them again at the next game step when they differ.
- `fire_rate` works (2026-10-22): `fn_8006C7B0` (equip) stores `1.0 / record+0x0C` as the weapon object's interval (weapon object +0x47C = player +0x4C4) and that interval sets the cadence, so writing only the record after equip did nothing. Measured with automatic fire over 150 frames: interval 1.0 gave 7 shots, 0.5 gave 14, 2.0 gave 4, 4.0 gave 2, 8.0 gave 1. The API writes the record and the live interval.
- Earlier note, superseded: fire_rate stayed `planned` (two approaches tried, no effect). `damage` is confirmed live (2026-10-22, autoplay bot, hp sampled per frame): zombies lose 10 per body hit, 3 or 5 per other hit, and with record 0 and 19 set to 100 the same hits take 100, 30 and 50 (the multipliers stay). `capacity`, `reload_time` and `fire_mode` are confirmed live too.

## Spare ammo (found 2026-10-22)

`fn_801BD014(table, set, key)` reads `u16` at `table + set * 42 + key * 2 + 0x5064`, with `table = 8056A2D0`, so the counts start at `8056F334`: 21 keys (0..20) per set, 42 bytes apart, set 0 for player 1 and set 1 for player 2 (my assumption for player 2; the game's reload passes 0). A second table at `+0x50B8` mirrors it (the setter `fn_801BD054` writes there; probably the maximum). The grenade word `8056F338` is key 2 and 3 of the table (the count is key 3). The weapon object's `+0x484` (player `+0x4CC`) is the ammo key; key 0 is the handgun's infinite reserve. Confirmed live: with the key forced to 7 (437 spare rounds in the author's save) a shot and a reload took the count to 436.

## Weapon equip (found 2026-10-22)

`fn_8006C7B0(weapon object, id, key)` stores the weapon id (`+0x480`), the key (`+0x484`), `0.0` at `+0x478` and the interval `1.0 / record[id].+0x0C` at `+0x47C`; see the `fire_rate` entry above.

## Camera as the player's position (found 2026-10-22)

The game is on rails and the player object barely changes (only a few floats near `+0x550`). The eye position is the camera's: `-R^T t` of the view matrix at `8044B4E0`, about 1.5 m high, with the zombies 3 to 6 m ahead along minus the third row. Exposed as `camera.position`, `forward`, `right` and `up`.
