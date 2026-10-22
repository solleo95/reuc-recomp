# Saves and mods

Mods that change how the game plays set `"touches_gameplay": true` in `mod.json`. While **any** enabled mod does, the game reads and writes a separate save folder, `saves-modded/`, instead of `saves/`.

- The first time it is needed, `saves-modded/` is created as a copy of `saves/`. After that the two folders go their own way; the modded copy is never refreshed from the normal saves.
- Switching all gameplay mods off returns the game to `saves/`. `saves-modded/` stays on disk for the next time.
- The normal saves are never written while a gameplay mod is active. If the copy cannot be made (disk full, no permission), the game says so and runs **without** the gameplay mods for that session instead of letting them touch the real saves.
- Mods that only change looks (textures, files, menu tabs) leave `touches_gameplay` at `false` and share the normal saves.
- The switch happens when the game starts: changing mods in the Mods tab needs a restart, which the tab says.
- A run with an explicit `--save-root` (the test harness) uses that folder and is never redirected.
