# Safety

Mods come from the internet. What the port does, and what it cannot do.

## Audio files

A mod's `audio/*.ogg` and `.wav` files are decoded by the port (stb_vorbis for Ogg) in the game process, with no sandbox of their own. A hostile or damaged file can crash the game, even for a mod that has no script or DLL. The port limits size (512 MB, 30 minutes) and checks every length it reads, but the Vorbis decoder is third-party code written for trusted input: see [audio.md](audio.md).

## Scripts (Lua, JavaScript)

Scripts run in a sandbox: no files, no processes, no network, no loading code from outside the mod folder (see [lua.md](lua.md) and [javascript.md](javascript.md) for the exact lists). They can only use the game API, their own settings, the log and the menu widgets.

Resource limits per mod: 1 ms per game step for all `on_step` handlers together, 4 ms per frame for menu drawing, 2 s to start, 64 MiB of memory. An endless loop is interrupted (JavaScript's interruption cannot be caught by the script). Three errors or overruns in a session switch the mod off until it is switched off and on again in the Mods tab (a mod that runs a minute of game time without trouble starts again from zero); the Mods tab shows the last error. The game keeps running.

Known limit: Lua's time check runs between instructions, so one long call into a library function cannot be interrupted.

## Native mods (DLLs)

A DLL runs with the player's full rights and can do anything the account can. No sandbox can change that. The port adds a gate:

1. **Allow native mods** in the Mods tab is off by default. With it off, only scripts and data run.
2. Turning it on shows a warning. Each DLL then needs its own confirmation; the port stores the file's SHA-256 and asks again when the file changes.
3. The DLL must report the API version and the Dear ImGui version it was built with; a mismatch is refused.
4. Files that are not DLLs are refused before Windows can show an error box.

A hardware exception inside a callback is caught and counts as a failure, and callbacks are timed, but a corrupt or hostile DLL cannot be contained. Only enable native mods from sources you trust. Prefer scripts when they are enough.

## Files and saves

- Every path in a mod is confined to the mod folder; zip entries that try to leave it (`..`, absolute paths) refuse the whole zip. Zips are limited to 20 000 entries and 512 MiB unpacked.
- Mods that change gameplay (`touches_gameplay`) play on the game's normal saves: there is no separate save folder, so a gameplay mod can change what ends up in a save. Back up `saves/` before trying one. The flag is enforced: a mod without it cannot change the game through the API, memory writes, data patches or hooks. Text (`text/`), files (`files/`) and the character names of `dol/names.json` are cosmetic and need no flag.
- What a mod sets in the game (switches such as `invulnerable`, weapon values) belongs to that mod: when it is switched off, reloaded or the game exits, the switches go off and weapon values return to the game's own. If two mods set the same weapon value, the last one wins.
- Mod files do not replace anything outside the game's disc tree and the `textures/` lookup.

## If something goes wrong

- Start the game with `--no-mods` to run without any mod.
- Delete or rename the mod's folder, or untick it in the Mods tab.
- The log is `debug/mods.log` next to the exe.
