# Native PC actions: feasibility and approved owner choice

2026-10-13. Owner correctly noted that Wii action and fire are separate buttons, and reload uses motion. Earlier proposed LMB/fire+confirm and RMB/reload+back described desired PC behavior, not an implementation by copying those two Wii button bits. Owner-approved bindings are recorded below; implementation plan approved for Native execution after the requested checkpoint commit.

## Verified local evidence

- `runtime/native_input.cpp`: real host pointer and held/pressed/released edges already reach the compiled game through KPADRead. `game/uc_main.cpp` currently maps physical LMB only to A0x0800; it must not be called complete gameplay shooting support. B0x0400 exists in the internal diagnostic protocol.
- Original DOL `8003B558` calls KPADRead; `8003C6C4` polls player channels. `80068DBC` selects the input channel object with stride0x19E4, offset0x14. Polling preserves separate button states and aggregates motion fields.
- `8003FB74` sets one of20 action masks; `8003FB98` reads it, checking its disable bit. `801F7D9C..801F7DC8` loads these masks through the setter. `80068454` separately checks pressed-state bindings4/5 and changes a selected index while skipping unavailable entries. This confirms a game-level binding/selection boundary; exact semantic labels for every index require further evidence.
- `80171E98` reads the motion axes using channel-specific accessors. `80171D38` compares motion strength/direction against caller thresholds and increments a discrete counter at object+0x10. Callers include `80067208`, `8006C268`, `800DBBBC` and `800DBC28`; the shared detector therefore must not be globally forced true by a reload key.
- `8006C268` branches on state+0x490, reads pressed binding0, checks the motion counter and calls `8006D980`. The latter checks limits/current amounts, invokes further state/event functions, writes amount+0x48C and enters state2. This is a promising reload candidate, **not yet accepted as a verified Reload entry**. Confirm with bounded runtime state/animation/ammo evidence before an HLE replacement.
- Static DOL table bytes alone are insufficient to label all live masks. Read back initialized state at the native boundary before assigning semantic action IDs. No Ghidra installation required for the evidence above.

## Recommended design

Host SDL input produces explicit Aim, Fire, Interact, Reload, ChangeWeapon, Grenade, Knife, Pause/Back and QTE actions. Publish actions at a game safe point under the existing scheduler baton; never modify game state from the SDL owner thread. Queue one-shot edges; preserve required hold/release behavior; release host-held input on focus loss.

Replace motion recognition at a **verified semantic consumer** with the real PC action, then retain the compiled game's normal animation, ammo checks, cooldowns and state transitions. Do not invent accelerometer samples or implement a Wii Remote trajectory/device model. Current neutral motion metadata remains TD-041 until the corresponding input consumers have native replacements.

Menu context maps LMB/Enter to Confirm and RMB/Esc to Back. Owner-approved gameplay maps LMB to Interact and RMB to Fire; Interact alone must not also fire. Resolve context from verified game state, never FPS thresholds or elapsed launch time. Keep ALT cursor release Windowed-only; Escape becomes Pause/Back under the approved control design.

Historical modern-PC proposal: LMB Fire, RMB or E Interact. Superseded by owner choice2026-10-13: LMB Interact, RMB Fire; R Reload; wheel and W/S ChangeWeapon; G or held Interact+Fire Grenade; held V repeats horizontal90-degree left-to-right Knife at the normal game interval, while held Interact+mouse movement preserves directional swings. E stays unbound. Owner requests multiple/clearable bindings in launcher and host-owned prompt icons without game-image changes. Detailed review artifact: `docs/superpowers/specs/2026-10-13-native-pc-controls-design.md`. Preserve QTE timing/challenge, avoid automatic success.

## Acceptance before calling controls complete

Verify menu/save actions do not fire; firearm shots consume magazine and hit enemies; holding automatic fire preserves weapon cadence; Reload starts the real animation with normal limits and cannot become Knife; weapon cycling skips unavailable weapons; Grenade costs an actual grenade; Knife direction/hold behavior and each encountered QTE remain functional. Test focus changes, short clicks, simultaneous inputs and queued commands exactly once. Replay through internal process-local input in a visible audible window with isolated save copies. Validate each supported image/profile independently.

No new runtime input implementation, dependency or emulation added by this investigation. Complete Reload/Knife/QTE behavior remains unverified; owner approved design and plan, with execution to begin after the requested checkpoint commit.
