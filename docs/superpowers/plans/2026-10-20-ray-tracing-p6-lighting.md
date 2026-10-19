# Ray tracing phase 6: ray-traced lighting (one diffuse bounce)

> Executed natively, test first, one fresh-context review at the end. Design: `docs/superpowers/specs/2026-10-19-ray-tracing-design.md` (effect 2); findings that shape it: S10 (the game's point lights are camera-relative), S12 (the frame supplies the hit colour). The owner asked to move on to lighting after the reflections.

**Goal.** `rt_lighting` (off/low/medium/high/ultra) adds one bounce of diffuse light: a surface brightens by the colour of the lit surfaces around it (the first scene's bright ceiling panel tints the seats under it, the red of a curtain bleeds a little onto the wall), softly and without any light of its own.

## Calls made here

1. **Same machinery as the reflections.** Cosine-weighted hemisphere rays (the AO directions) against the acceleration structure; a hit's colour is the frame's own colour at the hit's screen position when the hit is on screen and visible there (3% depth agreement, edge and distance fade); misses and hidden hits bring nothing. The effect is a mode of the reflection pass (one more instance of it with its own denoiser), not a new pass.
2. **Signal and composite.** The signal is the mean hit colour (an irradiance estimate, zero where nothing is hit); the composite adds `albedo x signal x strength` on GX-lit surfaces, faded by fog. Albedo is the RT albedo target (stage-0 texture x material colour, written while any RT effect is active). It adds light, it never darkens.
3. **The GX ambient term is not removed.** The design said the effect replaces `albedo x channel ambient`; the game's ambient is 0.04 on the first scene (nothing to double count), so it stays.
4. **No light of its own.** Only light that is already in the picture is redistributed; nothing is invented off screen (no sky, no environment), and the camera-relative lights mean the bounce follows them as the picture does.
5. Presets (divisor, rays, reach in view units, strength): low 4, 1, 8, 0.5; medium 2, 1, 12, 0.6; high 2, 1, 16, 0.7; ultra 1, 2, 24, 0.7.

## Tasks

### Task 31: math and presets (host)
`renderer/rt_light_math.h`: presets, the CPU estimate of the bounce (for the smoke fixture). `tests/test_rt_light_math.cpp`.

### Task 32: shader mode and pass instance
`rt_reflect.slang` gets a mode (reflection or lighting), an albedo binding for the composite; `RtReflectionPass` takes the mode in its inputs; a second instance and denoiser in the renderer; activation (`rt_light_active`, `rt_scene_active`); `GpuPass::rt_lighting` and the frame-log column. Smoke `--rt-lighting`: a dark floor beside a bright wall takes the wall's colour by the CPU estimate; the far floor does not; off, master off and a hidden hit change nothing.

### Task 33: UI. Launcher and in-game menu combo, translations.
### Task 34: measure, pairs, docs (S13), deploy.
### Task 35: one fresh review, fixes test first.
