# F5 — what depends on the game step rate (2026-10-17)

Owner report: some enemy animations and physics (falling lamps, pictures) run far too fast at a high frame limit. The game step above 60 Hz is the timer-driven
`game_hz` mode (F1/F3); the clock object 0x804D3318 then gets the real step in `+4/+8` (seconds) and `+0x20=1`.

## Finding

The clock holds two different notions of time:

| Field | Meaning | Users |
|---|---|---|
| `+4` / `+8` | seconds of this step (variable) | `dt*rate`-style code: correct at any rate |
| `+0x28` target, `+0x1C` rate | nominal frame = target / rate = 2 / 59.94 s (1/29.97) | ~20 functions compute `rate / float(target)` inline (`lwz +0x28`, int-to-float, `lfs +0x1C`, `fdivs`) |

Code of the second kind assumes exactly one call per nominal frame. The virtual `vtable+0x304` of every actor class (`800C790C`, 66 vtables) returns
`playback speed / nominal fps` and callers subtract/add it to animation/physics timers per call (`800CEE48`: `obj+0x878 -= f1`, `obj+0x870 += f1`). At
240 Hz those timers ran 8x too fast, at the 60 Hz step 2x.

## Fix

`hle_ActorStepTime` (`@800C790C`, `entry 800C7910`) lowers the `stwu`, runs the original body and multiplies its result by `game_step_scale()` = real step /
nominal frame (set by `hle_GameFrameWait` in the lowered modes only; 1.0 in the original 30 Hz mode, so that mode is byte-identical). Test: `test_game_pacing`.

Measured with the counter probe (below), chapter 1 first corridor, same state at 60 Hz and 240 Hz step, 2 game seconds each: words advancing by the same amount
per call (rate ratio 4, float words below 1e-6 are integers read as floats and left out) went from 78 floats + 9 ints to 0 floats + 9 ints.

## Reverted (2026-10-18)

`PropAge` (prop body counter and state interpolation, `80210278`), `UiAnimUpdate` (`8039C1D0`), `TimelineStep` (`80211734`) and the fractional `clock+0x24` were removed again: with them the owner saw no change in the
falling lamp / pictures / leeches above 60 Hz, and they could not be verified live. Kept: `ActorStepTime` (`800C790C`, measured: 78 per-call float words to 0) and `HudNameDelay` (`8014D368`, fixes `no data` instead
of the character name at >= ~450 Hz). The tables below describe the analysis of those counters; the wrapper names in them (other than the two kept) are historical.

## Inline `rate / float(target)` sites (classified, TD-055)

Read from the generated code (`generated/.../game_NNN.cpp`; the pattern is `lwz +0x28`, `lfs +0x1C`, `fdivs`). Result: **no further wrapper needed**.

| Site | Use | Verdict |
|---|---|---|
| `800A5A48`, `8010FC18` (clamped), `8012292C`, `80185CD8`, `8019C488` (x2), `801B0AAC` (x2), `80217FB8`, `8020D748`, `8024D358` (x3), `800FF660` (x2), `801E0ED0` | `clock+4 (dt) * rate/target` | frames elapsed this step: correct at any rate |
| `8003156C`, `801AEEE8` (+ callers `801AEFEC`, `801AF03C`), `8016B178` | getter of the nominal fps (TV format adjusted); callers divide a length in frames by it or multiply it by `+4` | unit conversion, independent of the step |
| `801FF414` | `-(rate/target) / x` with `x` a clip length read from the animation object (`obj+4`) | unit conversion |
| `8006EC8C` | `rate/target` times a constant time passed by three callers (`800702FC`, `800708A8`, `801CD750`) | evaluates a pose at a fixed time, one shot |
| `80046478`, `8036CF54`, `80388914`, `8038F394` ... | the `lwz 40` / `lfs 28` pair is a stack or struct access | not clock users (false hits of the first scan) |
| `80211734` | no clock access at all, but a scripted timeline player: its float clock (`this+42312`) grows by 1.0 per call and events fire at their frame | **per call**, found only by the roof probe (`805BDA08`). Wrapped: `hle_TimelineStep` |

## Integer frame counters (TD-055)

Writers resolved with `UC_WRITE_TRAP` and the generated code, consumers read in the same code:

| Counter | Writer / consumer | Verdict |
|---|---|---|
| `80B9EA14`, `80B9EA88`, `80E0C648`, `80E0C6BC` (+84 of four prop bodies, 0x74 apart) | `80210278` (physics substeps of falling lamps/pictures) ticks it per substep, resets it on contact; `800FA974` waits for age >= 2 / >= 20 calls to switch the prop state | **matters**: the state machine waited 0.67 s at 30 Hz, 83 ms at 240 Hz. Wrapped: `hle_PropAge` scales the ticks to nominal frames (carries the fraction per object) |
| `909A0928` and ~12 more words in the `9099xxxx`/`909Axxxx` pool (+80/+96/+136 frames and +112 state age of layout animation players) | `8039C1D0` advances them once per call | **matters**: HUD/menu animations ran 4x fast. Wrapped: `hle_UiAnimUpdate` runs the body every call and scales those four counters afterwards (skipping calls broke the callers: `guest trap ... indirect call` in `803AA7B0`) |
| `+7752` of the player HUD object (`8014D368`, one-shot delay of 10 calls before the player names are copied) | ticks once per call; invisible to the counter probe (it stops at 10) | **matters**: at >= ~450 Hz (`--fps-limit 0` = 480 Hz) the names were copied 9 steps after the stage start, before the player records were filled (about 20 ms of fixed time later), so the HUD showed `no data` instead of the name. Found by write-tracing the name table `804B79A4` at 360 Hz (fine) vs 480 Hz (broken): same writers, other order. Wrapped: `hle_HudNameDelay` parks the counter on calls that are not due |
| `805BDA08` (float clock of the timeline player `80211734`) | +1.0 per call | **matters**: scripted effects fired 4x early. Wrapped: `hle_TimelineStep` scales the tick afterwards (events fire at most one nominal frame early) |
| `80E0C5C0/C4` (ring index of a weapon trail, 8 entries of 136 bytes) | `8006DB34` (via `8006C26C`, the weapon update) pushes one sample per call; the fade (`+1184`) is dt-based | cosmetic: the trail is 4x shorter in time at 240 Hz. Not wrapped (the function may draw, skipping calls risks flicker) |
| `804AFB1C`, `806C3C38` (SDA throttle `r13-27112`) | `80129004` scans the pad/pointer state every 10th call | harmless: scans 4x more often (lower latency), no gameplay timing |
| `811305BC`, `92EE98E0`, `92EE990C` | floats advancing by 0.5 / 0.125 per step | dt-proportional (the integer probe matched their mantissa by chance) |
| `810385A0`, `81038778`, `81038950` | small floats written through `__fill_mem` (`80004338`) | unexplained, small amplitude, not wrapped |

Wrappers: `@80210278 PropAge` (`entry 8021027C`), `@8039C1D0 UiAnimUpdate` (`entry 8039C1D4`), `@80211734 TimelineStep` (`entry 80211738`), `@8014D368 HudNameDelay` (`entry 8014D36C`), all in `game/game_pacing.cpp`; with `game_step_scale()==1.0`
(original 30 Hz mode) they only lower the `stwu` and run the original body once, so that mode is unchanged. Test: `test_game_pacing`.

## Blind spot of the probe: short call-count delays

`UC_COUNTER_PROBE` finds words that tick by a constant amount for most of 240 steps. A delay that counts a handful of calls and stops (the HUD name delay above: 0..10) is invisible to it, and
such delays matter when the thing they wait for takes fixed time (resource fill, DVD). Finding method that worked: pick a visible symptom, find the memory word that holds it (string scan with
`peek`: `no data` was a literal in the guest), `UC_WRITE_TRAP` it at a good and a bad rate with `UC_WRITE_TRAP_ALL=1` (logs every write, with the game step number; resolve the guest chain with
`build/game/uc.map`), compare the order of the writers, read the code of the early one for a call counter. A deterministic run (`--deterministic`) makes the order reproducible (the plain run is racy
around the threshold). The tests used the harness `Game(game_args=[...--fps-limit N])`; `--fps-limit 0` is the 480 Hz ceiling, 360 Hz was fine.

Enemy ground speed checked at 30/60/240 Hz (deterministic, chapter 1 corridor, `enemies` from the frame state): the same zombies move 3.9/3.9/4.0 units per game second, so movement is rate-correct there.

## Timed step: retraces per step (2026-10-18)

Owner: 60 Hz is fine, 120 Hz already looks accelerated (lamp and picture falls, leeches). The 60 Hz mode is retrace-locked; above it `hle_GameFrameWait` runs the timed step and the tail
`80158208` stored `clock+0x24` (VI retraces this step stands for) = 1 at every rate. Code that counts in it (it reaches the HUD manager `801483D8` and the return value of `8003A6AC`) therefore ran
2x at 120 Hz, 4x at 240, 8x at 480. `+0x24` now accumulates `step * 60` and stores the whole part (0 on some steps). `PropAge` also interpolates the prop body state (`+32..+80`) to the real step.
Not verified live yet: lamp / picture / leech speed after this change (owner checks `fps_limit` 120 / 240). The counter probe cannot see accelerating motion (gravity) or call-count delays.

## State snapshots at equal game time (2026-10-18)

`UC_STATE_DUMP=<prefix>|1.0,1.5,2.0` (`game/state_dump.cpp`) writes the whole guest RAM at the first step that reaches each game time since `<prefix>.go`; `tools/spikes/state_diff.py <A> <B> <times>` lists the
float words that differ by more than 5% between two step rates. Used on "grenade at the chandelier" (deterministic, 120 vs 240 Hz): 82k words differ, nearly all in particle / cloth / random-number driven pools
(page-sized records at `80FA1000..81136000`, `9324E5xx`), so the lamp itself was not isolated. The method needs a scene without random effects, or a filter by object (find the object address first, then diff only it).

Status at this point: `fps_limit` 31..60 (retrace-locked logic) is correct per the owner; above 60 some per-call systems (falling lamp, pictures, leeches) are still too fast and the call-count based ones are not
findable by the counter probe. Options: per-system hunting with the snapshot diff, or logic at 60 Hz with frame interpolation in the renderer (not implemented).

## Probe results after the wrappers (TD-055)

Deterministic runs (`--deterministic`, virtual clock) compared at `--fps-limit 60` vs `240` (step 16.7 ms vs 4.17 ms). The probe scans all RAM every step, so a non-deterministic run drops to ~40 Hz and is useless;
from a bot-driven scene the pair is only comparable with `--deterministic` too. A scene without input stays at the first zombies (`settle` runs never advance), so later scenes need the bot (`autoplay` at x8 up to a retrace, then x1).

| Scene | Per-call (ratio 4) integers left |
|---|---|
| ch1 first corridor, 120 ticks after the start | `804AFB1C`, `806C3C38`, `80E0C5C0/C4` (+ the `__fill_mem` float triple, see above) |
| ch1 roof (bot, retrace 9600) | `804B003C`, `806C3C38`, `80E0C5C0/C4`; `8056A548`, `81002F60` and the `0.5 / 0.125` floats are dt-proportional |
| ch1 wave (bot, retrace 4500) | `804AFB1C`, `806C3C38`, `80E0C5C0/C4`; `81026FA0` is a dt-proportional float |

Chapter 2+ and the boss fight were not probed: the `after-ch1` checkpoint still fails at `save_slot_1`, and the boss scene needs a longer bot run. The remaining words are the input-scan
throttle (`80129004`) and the weapon trail ring, judged harmless/cosmetic above.

## Tools (diagnostics, all off by default)

- `UC_COUNTER_PROBE=<file>|<steps>` + `<file>.go`: snapshots guest RAM every game step and lists words that advance by a constant amount (`game/counter_probe.cpp`).
  Run it at two rates from the same state (`uctest` with `--deterministic --fps-limit 60|240`) and compare the amount per game second: ratio 4 = per call.
- `UC_WRITE_TRAP=<report>|<hex guest address>,...` + `<report>.go`: protects the pages (cached 0x80000000/0x90000000 view only; the aliases are separate
  mappings), logs host RVA, written address and the guest call chain (`game/write_trap.cpp`). Resolve host RVAs and guest return addresses with `build/game/uc.map`
  (`fn_<guest address>` symbols).
- `UC_PACKET_DUMP=<file>|<skip|go>|<frames>` lists every rendered packet; `UC_PACKET_STATS=<file>` is a histogram of draws by callsite/blend/texture/copy target.
  It identified the bloom composite (below).

## Bloom (owner item 3)

The glow around lamps is the post-process bloom of the game: the scene is copied to half size, blurred down/up through copies 7-16 (G21), then the helper
`800421B8` adds the result to the full frame with blend ONE/ONE (`gx::bloom_composite`). Skipping that one draw removes the glow (checked on the corridor lamp,
`--no-bloom`); the lamp flare sprites (`80357A68`) only feed the blur. Option: config `graphics.bloom` (default true), launcher toggle, `--no-bloom`.

## Prompts (owner item 1)

In-game prompts live in `ACT_All.brlyt` (panes `tex_A_Press`, `tex_B_Repeat`, `tex_R_Hshake`, ...), not only in the standalone `ACT_A/B_*` layouts, and the A+B
prompt in `ACT_AB_Press`. `PromptRegistry::actions` maps them: A/B to interact/fire (confirm/back in menus), A+B to the chord, `*_Hshake` to the mouse-shake prompt
(also while the scene gate reads as menu: the boss dodge prompt is drawn then; its box is now upright and as long as the remote picture).
Z, C, CZ and `ACT_N_Hshake` Nunchuk pictures stay original (the port has no Z/C action).

Open (TD-056): during the leech boss dodge (A+B and shake) `game_input_context` reads `menu` (scene suspension gate), where the mouse maps to confirm/back, so those
inputs may not reach the QTE; not exercised live.

## Owner item 2: static train scenery, window light, rain, thin smoke (2026-10-18, open)

Owner report (chapter 1, current save): through the windows the train's motion is not visible and the rain behind the window is static; in the dining car light
from the windows should run across the tables, and the smoke is thinner than in the original. Not explained yet. What is established:

- Reference: Dolphin with scripting (`D:\Tools\Dolphin-scripting`, `work\ctl2.py` = savestate load, screenshots, pointer/buttons, optional bot; `work\dctl.py launch|load|shot|bot`) runs the
  original. The first corridor and the dining car are reachable from `train3.sav` / `ref_r*.sav`; the Leech Man blocks the simple bot (needs aimed fire), so the roof is not reached.
  The original dining car shows dense haze over the whole car; ours is clearer (one frame each, not a controlled comparison).
- Not a step-rate effect that the counter probe sees. `UC_PACKET_MOTION=<file>|<frames>` + `<file>.go` logs per mesh draw (position matrix, texture matrix, indirect, colour, light hashes)
  and per non-mesh draw (vertex hash, texture matrix): at 30, 60 and 240 Hz no persistent mesh draw changes its texture matrix, indirect or colour state in the corridor, dining car or roof
  (positions change with the camera). Matrix ids 30/33/36 (projective/environment textures) do change with the camera, so dynamic matrices reach the renderer.
- Original GX stream (`build/train-oracle-seeded.dff`, 3 frames, corridor): 530 prims select texture matrices, 407 of them change between frames (camera driven); XF matrix-index
  register 0x1018 is written 17 times per frame; 333 prims at depth ~19.5 m use matrix id 0 (position matrix as texture matrix) with 3 textures. In our full playthrough no draw ever selects id 0
  (selectors seen: 30/33/36); a fix mapping GX_PNMTX ids to position matrices was written and tested but never triggered, so it was reverted.
- Next: record a longer Dolphin FIFO log (Tools > FIFO Recorder, ~60 frames) of the dining car or the corridor start and diff which GX state animates (texture matrices, texture images, BP
  colour registers) against our draw stream (`UC_PACKET_DUMP` / `UC_PACKET_MOTION`).
- 2026-10-18: `D:\Tools\Dolphin-scripting\work\dining_car.dff` (12 frames, via Tools > FIFO Player > Record; the main window needs a launch without `-b`) is INVALID: the player had died
  (god-mode bot was off), so it shows the death screen. Re-record with `bot on` running during the recording.
- Valid log: `D:\Tools\Dolphin-scripting\work\dining_car_live.dff` (12 frames, player alive, god-mode bot running during the recording, 22 MB).

### Finding: per-vertex texture matrix indices are ignored (2026-10-18, TD-058)

Original dining car frame (`dining_car_live.dff`): 1430 of 15138 primitives (9%) use per-vertex texture matrix indices (VCD `texNmtxidx`, mostly triangle strips of 4-8 vertices); between
frames 3422 texture-matrix slots change (rotation + translation, camera/projection driven) and 259 prims switch texture image. `uc_fifo_replay` refuses the log for the same reason
(`per-vertex texture matrix index unsupported`, `tools/fifo_replay/gx_state.cpp`).
Our live game: `Vertices::decode` stores `vertex.texture_matrix[]` (`renderer/gx_vertices.cpp:190`) but nothing reads it: `GpuMeshVertex`/`GpuVertex` have no field for it and the shader
(`generateTexcoord`) takes one matrix per texgen from the draw. In an autoplay run ~60 000 mesh draws (1%) have such descriptors (textures 1024x512, 512x512, 512x256, 256x256, 128x256,
2 TEV stages, blend 0:4:5), and the train-capture note G21 ("no per-vertex texture matrix indices") only held for the first car. A mesh that picks its matrix per vertex (several
projected light/environment matrices) is drawn with the single draw-level matrix, so the window-light movement across the tables, and probably part of the smoke/haze, are wrong.
Fix outline: pass the 8 per-vertex ids in the vertex (u8 x8 -> 2 uints), give TMI draws the texture matrix bank (126 x 3 float4, like PaletteBlock) and let the shader pick
`tex_rows` by id (id 60 = identity); keep the cheap path for draws without the descriptor. Verify against the original log with `uc_fifo_replay` once it accepts the stream.

### Fixed: raw display list calls were dropped (static rain, background, light; owner confirmed 2026-10-18)

- Cause: the game's material appliers (`80367FDC`, `80366FC0`, `80367AC4`, called from `80376874`) apply prebuilt display lists (texgen control `0x1040+n`, post `0x1050+n`, e.g. `0x5086` = MTX3x4 from the normal, post `0x103`)
  by writing the call command (`0x40`, address, length) straight to the FIFO. The hardware runs it, so Dolphin and the console see environment/specular texgens on ~74% of the scene draws; `uc.exe` dropped these
  writes (`outside_bytes`), so every draw kept the default UVi texgens. That removed the matrix-driven light/reflection that moves across tables, the window rain/reflection and the roof background.
- Fix: `runtime/hle/gx.cpp` `raw_display_list` (state machine in `gx_fifo_write`): the XF texgen loads of such a list are stored into the SDK block (`+0xC8`, `+0xE8`), which the native texgen reads. Test: `test_texture_matrix_hle`.
- How it was found: FIFO logs recorded in Dolphin (`window_rain*.dff`, `roof_rain.dff`) + `uc_fifo_replay --anim-report`; texgen display lists in guest RAM (`91CCxxxx`) dumped through a PowerPC trampoline patched in after a savestate load
  (`ctl2.py loadhook`); the missing call shown by a wrapper of `80376874`. Dolphin flattens display lists into its FIFO recording, which is why the recorded texgen words looked as if the SDK wrote them.
- Not done: other XF/BP loads inside raw-called lists are still ignored (`[raw-display-list] unhandled opcode` is logged once); `roof_rain.dff` does not replay yet (`partially recorded memory read`).

### Implemented: per-vertex texture matrix indices (2026-10-18, TD-058)

- Vertex: `GpuMeshVertex` 100 -> 108 bytes, `texture_matrix[8]` (u8, 255 = this vertex has no index for the texgen) read as one `R32G32_UINT` attribute at location 3.
- Draw: `DrawPacket::texture_bank` (`gx::TextureBank`, 126 x 3 float4, id 60 identity) exists only when `State.descriptors[1..8]` is set (`gx::has_texture_matrix_index`). Built from `texture_matrices` at the three draw
  creation sites in `runtime/hle/gx.cpp` (live, mesh, recorded/imported) and shared while unchanged; the renderer uploads one storage-buffer entry per distinct bank (descriptor binding 4) and passes its index in the push constant (`DrawPush.bank`, `0xFFFFFFFF` = none).
- Shader: `generateTexcoord` takes the rows from the bank when `bank != none` and the vertex's id for texgen n is < 126; otherwise the draw's own rows. Pipeline keys, warm-up seed and ubershader are unchanged (the branch is a push-constant test).
- Tests: `test_gx_vertices` (decode + `has_texture_matrix_index`), `renderer_smoke --validation` (id 5 shifts S by 0.5 and picks the other texel; id 255 keeps the texgen matrix); ctest 108/108.
- `uc_fifo_replay` no longer refuses the stream (bank from XF memory) and has `--allow-unresolved` (the log's frame 0 uses an EFB-copy texture that is copied later in the same frame; it is replaced by gray).
  `dining_car_live.dff` frame 3 renders (`uc_fifo_replay dining_car_live.dff 3 out.png --allow-unresolved`).


## 2026-10-20: prop integrator runs once per 60 Hz step above 60 Hz (PropStep)

`80210278` is a damped spring with per-call damping and an age counter. Measured with a body probe (`UC_PROP_PROBE=<file>`): the recorded motion lasted 2.24 s at 60 Hz, 0.95 s at 120, 0.72 s at 240. `hle_PropStep` (`entry 8021027C`) now skips the call while `2*game_step_scale() < 1` (accumulator per body), giving 66 executed steps at every rate (verified 60/120/240). 60 Hz and 30 Hz are unchanged (test_game_pacing). The earlier identification of this body as the visible chandelier was incorrect; its rendered object was not established. The train chandelier uses ODE, below.

Open (same method): the script step `800D027C` (event `smBh` at `800CCDAC` ends after 18 calls at any rate; gating the whole step loses one-call hit pulses, do not repeat), `800ACFA8`, `803946F0`, the +1 counters `804AFB1C`, `909A0928`... (UI), leeches, dogs. Tools: `-DUC_FN_COUNT=ON` probe build, per-step function log, burst-length classifier (compare run lengths in steps at 60 and 120 Hz: equal = per call).

## 2026-10-21: confirmed chandelier ODE timestep and shot impulse

The visible `r010_chandelier` resolves from model node `80E9C878` to actor `80E9B180` (vtable `803F7EA0`, body ID 0 at +3252). ODE manager `804A197C` stores 44-byte records at its +24 pointer; record ID +40 maps to the body pointer at +0. In the corridor replay this is `933DC230`, position +152 and rotation +184. The visible matrices are `80E9CC80` / `80E9CE60`; writes through the physical alias explain why a trap on only the cached address missed them.

`80159BC0` passes a fixed `2 / clock.rate` seconds to `801172A4` on every game step. That function divides it into four ODE substeps (`800EC6F8` -> `800F215C` -> `800EE300`). Thus rigid bodies ran twice as fast at 60 Hz, four times at 120 Hz. `hle_PhysicsStep` scales that duration by the actual/nominal frame ratio and skips the solver during pause. Original 30 Hz remains unchanged.

Shots enter `80117464` from `8010000C` at `80100938` and add force/torque for one solver substep. `hle_PhysicsHit` inversely scales only this event's strength, preserving the original impulse when the substep shrinks; generic forces remain untouched. Tests cover elapsed physics time and shot impulse at 30/60/120/240/480 Hz, plus pause. Removing either normalization makes the corresponding test fail.

An identical `cg60` replay before/after the timestep fix has matching matrices before the hit and corrected rotation/fall afterward (`demofn-codex_state60-1792558666`, `demofn-codex_ode60-1792559059`). Separate unlocked 120/240 Hz harness runs capture the body position and rotation through repeated hits.

Pictures are confirmed ODE clients too: actors `8109F900`, `810A5920`, `810AB940`, `810B1960` have body IDs 9..12 and share model resource `92DE4AE0`, MDL0 name `r008_picture` (resource pointer at actor +6216). Targeted harness shots at 120/240 Hz record picture rotation and falling to the floor in `pictures-120-1792561619452566300` and `pictures-240-1792561550176317600`. These are live aiming runs, not identical collision trajectories: one picture catches on the rail before falling, so time to final rest is not a clean timing benchmark.

## 2026-10-21: actor timestep was scaled twice for animated enemies

The earlier assumption that every `800C790C` result is a fixed nominal timestep was wrong. In live 120 Hz gameplay, the original body returns `0.008333334` for classes `803F2B60` (zombies), `80408534` and `80411484`, but `0.0333667` for the plain props (`803F7EA0`). Multiplying all results by `game_step_scale() == 0.24975` made enemy AI/state timing four times slower than their already-correct animation and root-motion paths.

The animated model getter `80032AA0` reads a nested playback value already containing elapsed time: `0.24975` at 120 Hz, `1.4985` when the accelerated test uses a 50 ms step. Both `8016B178` and `800312CC` divide this by the nominal 29.97 Hz, recovering actual elapsed time. Experimental extra scaling in those two paths was rejected by live probes and removed.

`ActorStepTime` now preserves the original result and only enforces host pause. `FixedPlaybackStep` wraps the plain model getter `800329FC` (its own +16 playback property), applying the frame ratio only when called from the original actor-timestep division (`LR 800C799C`). Other property consumers and animated models keep their original values. This preserves the previous correction for fixed-step props while removing the duplicate factor from enemies. The regression test fails with the old blanket multiplier and checks animated/fixed paths, pause and unrelated getter callers at 60/120/240/480 Hz.

Evidence: `build/actor-time120.txt`, `build/autotest/live-120-135-1792560788729229700`; probe columns are game step, actor, class, caller, original timestep, frame ratio. Temporary timing probes were removed from production sources.

Post-fix live checks: roof leeches approach and attack at 120/240 Hz (`live-120-140-1792561336855142000`, `live-240-140-1792561432162952300`); the first-car zombies approach and reach the player at 480 Hz (`live-480-20-1792561675445062200`). The route, camera phase and hit results vary with live autoplay, so these establish working behavior rather than bit-identical replay. Full build and CTest: 134/134 passed (`build/actor-fixed-build.log`).

The unblocked zombie #162 covers about 22.7 world units over six game seconds in the 30 FPS reference (`live-30-20-1792561731127962200`) and 22.5 at 480 Hz, about 3.8 units/s in both. Zombie #163 moves from the seat row (z=10.14) into the aisle (z=-0.16) in the 480 Hz check. These checks do not cover every enemy type or every navigation state; the reported dog-against-wall case still needs a dedicated scene check.

Additional progression smoke: `live-120-600-1792562171777636200` reached the first train chapter's results and `chapter_done` at retrace 32718. The diagnostic script exits with `route stopped: chapter_done` because its requested 600-second capture lies after the chapter ends; this is not a game crash. This route used speed 8 (50 ms effective game steps), so it proves chapter progression only; the targeted high-frequency checks above switch to speed 1. The dog-against-wall scene was not identified by this route or by the preceding Claude session notes.
