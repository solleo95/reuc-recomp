# Ray tracing phase 4: ray-traced ambient occlusion

> Executed natively in this session (owner's choice since phase 2), test first, one fresh-context review at the end. Design: `docs/superpowers/specs/2026-10-19-ray-tracing-design.md` (effect 5, presets, replacement of GTAO); findings that change how it applies: `docs/spikes/S10-rt-level-shadows.md`.

**Goal.** `rt_ambient_occlusion` (off/low/medium/high/ultra) darkens creases and contact areas by rays against the scene's acceleration structure, replaces GTAO while active (already encoded in `graphics_effects.h`), shows in the launcher and the in-game menu, and costs what the phase budget allows (High: all ray tracing about 2.9 ms on the RTX 4080; AO about 0.3 ms at High).

## Calls made here (the owner delegated this phase)

1. **AO multiplies the whole lit colour, not the "ambient share".** The design said ambient share, but the game's ambient is tiny (0.04 on the first scene's seats; the light comes from lights and baked colour), so an ambient-share AO would do nothing. GTAO (the effect it replaces) multiplies the whole scene colour by `lerp(1, ao, strength * (1 - fog))`; RT AO does the same, on every depth-writing surface with a normal that is not a cut-out (no model-valid gate: AO needs no light model).
2. **One signal.** The trace pass writes `shadow factor x AO factor` into the existing signal; the same denoiser and composite handle it. The composite no longer gates on the light model (the trace writes 1 where an effect does not apply). Divisor = the finest of the active effects' divisors; rays and radius from the AO preset.
3. **Cosine-weighted hemisphere rays**, first hit within `radius` counts as blocked (any-hit, binary), mask static + skinned (characters occlude too). Occlusion = blocked / rays; factor = `1 - strength * occlusion`. No hit-distance weighting (the denoiser and the rays' count give the softness).
4. **No new push constants**: `lampClearance` becomes a shader constant (it is only tuned in `rt_shadow_math.h`, mirrored by a test), freeing the slots for `aoRays`, `aoRadius`, `aoStrength`.
5. Records and the RT material pair run for any active RT effect already (`any_rt_active`); AO-only frames build the AS but skip the shadow branch (no record for pixels without lights is harmless).

## Tasks

### Task 20: AO math and presets (host)
`renderer/rt_ao_math.h`: `RtAoPreset {divisor, rays, radius, strength}` per level (low: ½ res, 1 ray, 0.35; medium: ½, 1, 0.5; high: full, 2, 0.5; ultra: full, 4, 0.8 — the design's table, radii in view units like GTAO's 0.5), `rt_ao_factor(blocked_fraction)`, `rt_ao_direction(normal, u1, u2)` (cosine-weighted, unit, in the normal's hemisphere). Tests `tests/test_rt_ao_math.cpp`: preset table, direction unit length and `dot(n, d) >= 0` over a grid of samples, mean cosine of 1/2..2/3 (cosine weighting), factor endpoints.

### Task 21: Trace and composite
`shaders/rt_shadow.slang` (AO branch, composite gating), `rt_shadow_pass.cpp/h` (`RtShadowInputs.ao`, divisor, push), `vulkan_renderer.*` (`rt_shadows_active()` becomes `rt_effects_active()` including AO; the pass call and the TLAS condition follow; GTAO skip stays). Smoke `--rt-ao`: a floor and a perpendicular wall (a crease) under rt_ambient_occlusion only: the signal at a floor point at distance d from the wall matches the CPU estimate of the occluded fraction of the cosine hemisphere (within the ray noise: average over frames), far floor stays 1; with only AO on, no shadow branch runs; GTAO does not run while RT AO is on; off allocates nothing; validation clean.

### Task 22: UI and options
Launcher and in-game menu combos (greyed with the master like the others), translations, `rt_ui` (AO replaces GTAO: grey the raster AO combo with "replaced by ray tracing"), CLI and config already exist. Tests `test_rt_ui`, `test_options*`, `test_rt_effects`.

### Task 23: Measure, live pairs, docs, deploy
fight3 per preset (off, low..ultra), the first scene's matched pairs RT AO vs GTAO vs none, tune radius and strength, `docs/spikes/S11-rt-ao.md`, `docs/graphics-effects.md`, `build.bat` deploy, commit.

### Task 24: Review and fix pass
One fresh review of the phase (focus: AO applied to unlit/baked surfaces and HUD-like draws, characters over-darkened, GTAO replacement, denoiser history with two signals folded, cost when only AO is on, push-constant layout), fixes test first, deferred minors listed.
