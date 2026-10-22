# M5: the game's text files for mods (milestone 2 of the asset mods design)

Checked 2026-10-23 (USA build, harness at 1280x720). The file layout is in `docs/superpowers/plans/2026-10-23-asset-mods-text.md`.

- All eight `mes_<lang>.txtres` of the owner's install parse (2634 records; `mes_du` 1859), patching without changes gives identical bytes, and a patched record reads back.
- The game opens `Data/message/mes_us.txtres` once at start and reads the whole file in one request (length rounded up to 32 bytes), into MEM2 (about `0x91240000`); the pool appended by the patcher is part of that read. `--log-dvd` now also prints every read (`[dvd] read <path> offset N length N`).
- Live checks: `uctest run text_replace` (mod `text_marker` sets records 500, 550 and 553 for every language) finds the marker text in the game's memory next to the original string, `text_replace_plain` finds only the original; the HUD name of the handgun in the train chapter shows `UCTEXTMARKER` (capture checked by eye).
- `mes_us` of this install is a Russian localization drawn with the font's own code points (`Handgun` is `&ucmovem`). Records 500, 550 and 553 are the handgun's name in the HUD at the start of the train chapter (the Russian file keeps the original `Samurai Edge 2` names as `Kvuèok cawypaô 2`).
- Not checked: whether longer lines than the originals fit the game's text boxes and layout buffers (the limit of 2048 bytes per line is the port's, not the game's; a text beyond twice the original maxima of 252 bytes and 12 lines is reported in the Mods tab), and a multi-line record whose line count changes (unit tested only).
