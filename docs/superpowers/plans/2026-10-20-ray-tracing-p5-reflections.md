# Ray tracing phase 5: ray-traced reflections

> Executed natively (owner's choice since phase 2), test first, one fresh-context review at the end. Design: `docs/superpowers/specs/2026-10-19-ray-tracing-design.md` (effect 1, presets, RR inputs). The owner delegated the calls of this phase ("начинай отражения: дизайн, план, реализация").

**Goal.** `rt_reflections` (off/low/medium/high/ultra) adds reflections of the scene on the surfaces the game itself marks as glossy, replaces nothing (SSR was never built), shows in the launcher and the in-game menu, and costs what the budget allows (all ray tracing at High about 2.9 ms on the RTX 4080).

## What the game gives us (survey of the first scene, 2026-10-20)

* No material list, no environment map. The one gloss signal is the specular channel: draws with channel 1 lit with the specular function carry the specular material colour in `materials[1]`: 0 (matte), 0.02 to 0.10 (the common wood/metal class `ch0=71E`), 1.0 (the seat/floor/wall class `ch0=3F1E`, where a texture times the specular modulates it: the strength is in a texture we do not sample). 33% of the first scene's indices have it lit, 40% have it unlit (baked).
* Hit shading needs a colour for a ray's hit. The acceleration structure holds positions only (no UVs, no textures), so a hit has no colour of its own.

## Calls made here

1. **Reflectivity** of a draw = `0.5 * sqrt(luma(materials[1]))` when channel 1 is lit with the specular function, else 0 (`rt_reflectivity`): 0.02 to 0.10 gives 0.07 to 0.16, 1.0 gives 0.5. Stored in bits 12 to 19 of the record flags (8 bits), `kRtReflective` set when above 0. A Schlick Fresnel (F0 0.04) multiplies it. No `dist/rt_materials.json` (design option): per-material overrides wait for a case that needs them.
2. **Hit colour from the frame.** A ray's hit point is projected back to the screen; its colour is read from the finished scene colour (shadows and AO already applied) when the hit is on screen and its depth matches the depth buffer there (within 3% of the distance): then the reflection is what the camera itself sees at that place, correctly lit by the game. A hit that is off screen, or hidden from the camera (its pixel shows something nearer), or a miss, gives no reflection (confidence 0; the surface keeps its colour): no sky, no environment, no invented colour. This is a ray-traced screen-space hybrid: rays decide visibility and the hit, the frame supplies the colour. Limitation (documented): things the camera cannot see are not reflected.
3. **Radiance signal, own denoiser.** The trace writes RGB radiance and the hit confidence into an RGBA16F image at the preset's divisor; a second `RtDenoiser` (`RtSignal::radiance`, variance-guided) filters it; a composite lerps the colour toward it: `color = lerp(color, reflection, weight)`, `weight = fresnel * reflectivity * confidence * (1 - fog)`, on lit, opaque surfaces with a normal.
4. **Order.** After the shadows and AO composites (the reflection reads the colour they finished), before bloom/upscale like them. Uses the same TLAS (masks 3: level and characters), the G-buffer, the material pair and the records.
5. Presets (divisor, rays, cone half angle in radians, reach in view units): low 4, 1, 0, 30; medium 2, 1, 0, 45; high 2, 1, 0.05, 60; ultra 1, 2, 0.08, 80.

## Tasks

### Task 25: reflection math and presets (host)
`renderer/rt_reflect_math.h`: presets; `rt_reflectivity(material, specular_lit)`; Schlick; `rt_reflect(view_dir, normal)`; view-position to pixel projection (the inverse of `viewPositionAt`); the flag packing (`rt_record_flags` gains reflectivity). Tests `tests/test_rt_reflect_math.cpp`, `test_rt_material.cpp`.

### Task 26: records and activation
`vulkan_renderer.cpp` record fill passes `materials[1]` and channel 1; `rt_trace_active` and `rt_scene_active` include reflections; `rt_material_active` already follows `any_rt_active`.

### Task 27: pass, shader, denoiser
`renderer/rt_reflection_pass.cpp/.h`, `shaders/rt_reflect.slang` (trace, composite), a second denoiser instance in the renderer, `GpuPass::rt_reflections` timestamp, build wiring. Smoke `--rt-reflections`: a glossy floor under a bright coloured box (the floor point below the box shows the box's colour weighted by fresnel and reflectivity; a matte floor does not; a box hidden behind another surface gives nothing; off allocates nothing; validation clean; moving camera test as in the shadow pass).

### Task 28: UI
Launcher and in-game menu combos, translations, tests.

### Task 29: measure, live pairs, docs (S12), deploy.
### Task 30: one fresh review, fixes test first.
