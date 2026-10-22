# Mod API changelog

The API is versioned `major.minor`. A new minor version only adds members or makes `planned` ones real; a new major version may remove or change things. A mod declares the version it was written for (`"api"` in `mod.json`) and is refused when the game's major version differs or its minor version is older.

## 1.0

First version.

- Scripts: Lua 5.4 and JavaScript (QuickJS-ng), native DLLs (`uc_mod.h`).
- Domains: `mem`, `symbols`, `player`, `weapon`, `camera`, `enemies`, `hook`, plus the per-mod `log`, `settings`, `ui` and `on` events.
- Implemented players: presence, magazine, grenades, damage meter, `invulnerable`, `infinite_ammo`; events `on_weapon_fired`, `on_reload`, `on_damage_taken`.
- Implemented weapons (by record id 0 to 19): `capacity`, `reload_time`, `fire_mode`; `damage` is `unverified`.
- Enemies: zombies only (id, class, position, hp).
- Hooks: `hook.pre` on `GameWeaponUpdate` and `PlayerEventHandler` (`unverified`: unit-tested, not exercised in a live fight).
- Not yet: `player.position`, `weapon.fire_rate`, `input.fire_held`, `hook.post`, `hook.replace`.
