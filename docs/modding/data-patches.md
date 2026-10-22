# Data patches

A data patch changes game values without any code. Put one or more JSON files into `mods/<id>/data/`; each is one object that maps API property names to values:

```json
{
  "weapon.0.damage": 25,
  "weapon.0.fire_mode": "auto",
  "weapon.2.capacity": 80,
  "player.1.invulnerable": true
}
```

## Keys

`<domain>.<entity>.<member>` for a domain addressed by a number (`weapon.<id>`, `player.<slot>`), or `<domain>.<member>` for a global one. They are the same names as in the [API reference](api/README.md): `weapon.0.damage` is `uc.weapon(0).damage`.

A key must name an **implemented** member that can be written. The value goes through the member's setter exactly as a script's assignment would, so the same limits apply (a weapon's capacity is a whole number from 0 to 999, damage is a number from 0 to 10000, and so on), and weapon values are kept for the whole session even when the game refills its tables.

Keys that do not work are skipped and logged with the file name and the reason, and the rest of the file still applies:

- unknown or not implemented yet (`planned` members, such as `hook.post`, are listed in the reference but cannot be patched);
- read-only members (`camera.view`);
- an entity that is not a whole number, or a missing or extra entity part;
- a value of the wrong type or outside the allowed range.

## Order

Files in `data/` apply in name order; keys apply in the order they appear in the file; mods apply in load order. When two patches set the same value, the last one wins. Patches apply once when the mod starts, after its script's `on_load`.

A mod may consist of data files only: it needs no `script`. Such a mod that changes gameplay still sets `"touches_gameplay": true`.

Files over 1 MiB, files that are not valid JSON and files whose root is not an object are skipped with a message.
