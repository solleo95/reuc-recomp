# Saves and mods

Mods that change how the game plays set `"touches_gameplay": true` in `mod.json`. While **any** enabled mod does, the game reads and writes a separate save folder, `saves-modded/`, instead of `saves/`.

- The first time it is needed, `saves-modded/` is created as a copy of `saves/`. After that the two folders go their own way; the modded copy is never refreshed from the normal saves.
- Switching all gameplay mods off before the next start returns the game to `saves/`. `saves-modded/` stays on disk for the next time.
- Mods can be switched on while the game runs. The first time a gameplay mod is switched on in a session, the game moves to `saves-modded/` (created from `saves/` as they are at that moment) as soon as no save file is open, and **stays there until the game is restarted**, even if the mod is switched off again: the game's memory may already hold what the mod did, and that must not be written to the normal saves.
- The normal saves are never written while a gameplay mod is active. If the copy cannot be made (disk full, no permission), the game says so and runs **without** the gameplay mods for that session instead of letting them touch the real saves.
- Mods that only change looks (textures, files, menu tabs) leave `touches_gameplay` at `false` and share the normal saves.
- If the copy cannot be made when a gameplay mod is switched on live, the mod does not run and the Mods tab says why.
- A run with an explicit `--save-root` (the test harness) uses that folder and is never redirected.
