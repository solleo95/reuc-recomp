# M6: rebuilt U8 archives for mods (milestone 3 of the asset mods design)

Checked 2026-10-23 (USA build, harness at 1280x720, `uctest run arc_grow`, which is `train_gameplay` with the generated mod `scripts/uctest/mods/arc_grow`). Tools: `tools/spikes/u8_repack.py` (parse, rebuild, `roundtrip`) and `tools/spikes/u8_grow_mod.py` (writes the mod: one archive with one member grown by N zero bytes; the mod folder is git-ignored).

## The archives

- The owner's install has 614 `.arc` files (371 MiB). All are plain, uncompressed U8; none is Yaz0 or LZ77 compressed. The data area starts at a multiple of 0x20, members are 0x20 aligned and packed in offset order without gaps.
- A parse and rebuild (same node and string tables, members repacked in their order) gives **byte-identical** files for all 614 archives. So a rebuild with one member replaced changes nothing else.
- Two files, `script/zzz_t102.arc` and `script/zzz_test.arc` (leftover test archives), end 12 bytes before their last member's declared end. The builder must reject a member that reaches past the end of its archive instead of silently shortening it.
- A train chapter run opens 55 archives (layout, script, effect, sysres, chara, bg). The game reads each one in full, in 0x60000 (393216) byte chunks, at the length the DVD layer reports; the overlay's length is used as is.

## Size safety (the spike's question)

- **The loaders follow the file's own tables.** Moving the data area 0x40 bytes away from the tables (so every offset changes) loads fine in `sysres_g.arc`, `layout/game_us.arc`, `effect/ef_com.arc` and `script/r_biouc.arc`. Members that grow by about 4.7 KB (later members move) load fine in the same four. So adding a member (bigger tables) and replacing a member by a different length are both safe as far as the format goes.
- **Memory is the limit.** A bigger archive needs a bigger buffer from the game's own heap, and when that is not available the game stops with `fatal: DVDReadPrio into an invalid guest buffer 00000000+393216` (a null buffer from the failed allocation of the 0x60000 chunk, in `JKRDvdRipper`/`JKRAram` code). The headroom differs by archive, because it is the free space of the heap that archive loads into:

| Archive (one member grown) | grew by | result |
|---|---|---|
| `layout/game_us.arc` (2.4 MiB) | 0.25 MiB / 0.5 MiB | loads / game stops |
| `sysres_g.arc` (1.0 MiB) | 0.25 MiB / 1 MiB | loads / game stops |
| `effect/ef_com.arc` (3.1 MiB) | 1 MiB / 2 MiB / 4 MiB | game stops (all three; the 1 MiB run ended as a lost harness pipe, the game having exited) |
| `script/s00c1.arc` (0.56 MiB) | 1 MiB | game stops |
| `chara/EZ01.arc` (0.6 MiB, zombie model) | 1 MiB / 2 MiB / 4 MiB | loads / game stops / game stops |
| `bg/r000bgobj.arc` (25 KB) | 1 MiB | loads |

  Archives of the always-loaded layers (layout, sysres, script) have a few hundred KB of room; stage archives (chara, bg) have around a MiB. Nothing is known about the exact limit beyond these bounds, and fragmentation can move it.
- The port has no spare guest memory to give: the guest is the console's 24 MB + 64 MB and MEM2's arena already ends at the console's value.

## What follows for the feature

- Archive member replacement is feasible: the rebuild is exact and the loaders accept other sizes and other table sizes.
- Authors must be told that the **size of the rebuilt archive matters**, not only the member: replacements near the original size are safe; growing an archive by more than about 256 KB is risky and by more than about 1 MB is likely to stop the game. The Mods tab should show the growth of every rebuilt archive and warn above 256 KB.
- A crash from this cause should name the file: the DVD layer can say "the game could not allocate a buffer for `<path>` (a mod made it N bytes bigger than the original)" instead of the bare null-buffer message.
- Not checked: shrinking a member (obviously fine for the DVD layer, content not exercised), archives nested inside archives, and which archives the rest of the game (chapters 2 to 4) loads.
