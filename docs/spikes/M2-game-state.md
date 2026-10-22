# M2: game state for the mod API

Found 2026-10-22 with the harness (`uctest run state_dump`: paused word dumps while the autoplay bot plays the train chapter) and the Dolphin Gecko codes in `memory_cheat_codes.txt` (the USA addresses are the PAL ones minus 0x300). Guest addresses, USA build.

| What | Address | Notes |
|---|---|---|
| Enemy hits, P1 / P2 | `805C5CC0` / `805C5CC4` | u32, rose with every hit (39 after 105 s of bot play) |
| Critical hits, P1 / P2 | `805C5CC8` / `805C5CCC` | u32, from the Gecko code (the bot made none) |
| Objects destroyed, P1 / P2 | `805C5CD0` / `805C5CD4` | u32, rose 0 to 5 in the train chapter |
| Mission clock | `805C5D74` | u32 whole seconds, about one per second of game time; `805C5D70` is a float that wraps in 0..1 (not exposed) |
| Stars (upgrade money) | `804C77C4` | u32; unchanged during a mission |
| Pause state | `805965A8` | 2 while the pause panel is open (also used by the input context); `805965AC` is the scene substate |

Not found:

- Chapter / mission id: no stable word. A diff of 0x80400000..0x80600000 between two chapter-1 runs and one chapter-2 run left only actor tables, resource names (`MV_002_0` for chapter 2's movie) and counters that differ per run. The scene object's +0x74 / +0xE8 hold an object sequence number (22, 23, 11, 41, 32 over five runs), not a stage id.
- Difficulty, score, rank: not looked for beyond the diff above (the result screen was not reached: god mode, bot stops).
- Kills: the game keeps no kill counter (enemy hits is a hit counter); the API derives kills from `on_enemy_killed`.
