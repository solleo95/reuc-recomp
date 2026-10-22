# Mod API changelog

The API is versioned `major.minor`. A new minor version only adds members or makes `planned` ones real; a new major version may remove or change things. A mod declares the version it was written for (`"api"` in `mod.json`) and is refused when the game's major version differs or its minor version is older.

## 1.1

- `hook.post` and `hook.replace` (Lua, JavaScript, native `hook_post` / `hook_replace`): code after a hookable function or instead of it. `UC_MOD_API_MINOR` is 1.
- `weapon.fire_rate` and a confirmed `weapon.damage` (both were announced for 1.0 but only checked later).
- `input.held`, `input.fire_held`, `input.aim` (what the game sees on player 1's pad) and `input.force_held`, `input.block_held`, `input.aim_override` (add to or take away from it).
- `player.weapon` (held weapon id), `player.health` (0 to 1), `player.reserve` (spare ammo of the held weapon; nil for the handgun).
- `uc.enemy(id)` with `hp` (read and write), `alive`, `head`, `position` (read and write); zombies only.
- Events `on_enemy_spawned`, `on_enemy_killed`, `on_enemy_headshot`, `on_player_died`, derived from state changes between ticks.
- `game.time` (mission clock, seconds), `game.paused`, `game.stars`, and the per-player result counters `player.hits`, `player.critical_hits`, `player.objects_destroyed`. Not offered yet: chapter or mission id, difficulty, score and rank (their places in memory are not found).
- `uc.guest.call(address, args)`: runs a translated game function with integer arguments and returns r3. Works only from the game's own events (on_step, the game events, hook handlers). `uc.game.give_weapon(slot, id, key, magazine)` and `uc.game.select_weapon(slot, index)` on top of it (checked in the game: a mod gave the player a submachine gun and switched to it). Not offered yet: spawning enemies and items (no spawn function identified).
- JavaScript only: `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval` on game time (steps, paused with the game); see [JavaScript](javascript.md#timers).
- `camera.position`, `camera.forward`, `camera.right`, `camera.up`, computed from the view matrix. `player.position` is not offered: the game is on rails and both players share the camera, so the camera is the player's viewpoint.

## 1.0

First version.

- Scripts: Lua 5.4 and JavaScript (QuickJS-ng), native DLLs (`uc_mod.h`).
- Domains: `mem`, `symbols`, `player`, `weapon`, `camera`, `enemies`, `hook`, plus the per-mod `log`, `settings`, `ui` and `on` events.
- Implemented players: presence, magazine, grenades, damage meter, `invulnerable`, `infinite_ammo`; events `on_weapon_fired`, `on_reload`, `on_damage_taken`.
- Implemented weapons (by record id 0 to 19): `capacity`, `reload_time`, `fire_mode`, `damage` (checked: with 100 a handgun hit takes 100 off a zombie), `fire_rate` (checked: shots in a fixed time follow the rate).
- Enemies: zombies only (id, class, position, hp).
- Hooks: `hook.pre` on `GameWeaponUpdate` (checked in the game: r3 is the weapon object, clearing r4 stops firing) and `PlayerEventHandler` (`unverified`: unit-tested, not exercised in a live fight).
- Not yet: nothing announced.
