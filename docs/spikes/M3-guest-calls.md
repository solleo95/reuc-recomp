# M3: calling game functions from mods

Found 2026-10-22 (USA build). Guest addresses.

## Mechanism

A call needs the register file of the guest thread, so `mods_step` now passes the `ppc::Ctx` of the pacing function to the host, and `run_hook_slot` hands over the one of the hooked function; both open a `GuestCallScope` (modding/mod_guest_call.h). `uc.guest.call` copies the Ctx, loads r3..r10, runs `uc::call_indirect` (the game registers the backend in `uc_main.cpp`, so the unit tests can use a fake) and restores the Ctx. Calls from other threads (the settings window) and to addresses that are no function entry are errors; nesting is limited to 4.

## Weapon loadout (player object, `806C3AD8` / `806C3ADC`)

Read from `fn_80067980(player, slot)` (the slot switch) and a live dump of the train chapter:

| Offset | What |
|---|---|
| `+0x6C4 + 4 * i` | magazine of loadout slot i (16 slots) |
| `+0x704 + 4 * i` | weapon id, `0xFFFFFFFF` = empty |
| `+0x744 + 4 * i` | ammo key (index into the spare ammo table) |
| `+0x784` | the slot in use |

`fn_80067980(player, slot)` clamps the slot (negative: 15, 16 or more: 0, an empty slot: 0), stores it at `+0x784` and equips the slot's weapon through `fn_8006C7B0`. Checked live: `give_weapon(1, 2, 7)` then `select_weapon(1, index)` makes the submachine gun the player's weapon (HUD name, reserve 406 from key 7, automatic fire uses the magazine). The weapon records hold no ammo key, so `give_weapon` takes it as an argument (default 0, infinite reserve).

## Not done

- Spawning enemies and items: no spawn function was identified. Waves are script-driven by the scene; finding the spawner needs tracing a wave start.
- Item pickups (first aid, ammo boxes) are not called either.
