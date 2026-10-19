# Ray tracing (design)

Date: 2026-10-19. Status: draft for the owner's review.

## Intent

The port already has raster enhancements on top of the Wii picture (`docs/superpowers/specs/2026-10-19-graphics-enhancements-design.md`,
`docs/graphics-effects.md`): GTAO, a shadow map of the dominant directional light, contact shadows, per-pixel lighting, motion blur. The owner
wants a ray-traced tier on top: five ray-traced effects with quality presets, one master switch, and NVIDIA DLSS Ray Reconstruction as the
denoiser where it runs. Plain ray tracing only (no path tracing).

The owner's goal on the owner's RTX 4080: every ray-traced option on **High** at about **100 FPS** (first stated as 120; lowered by the owner after the phase 0 measurement, 2026-10-19); everything on **Ultra** at no less than **60 FPS**.

## Options (owner's decisions)

One master checkbox and five options, each `off`, `low`, `medium`, `high`, `ultra`:

| # | Option (UI, ru) | Config `graphics.*` | CLI | What it is |
|---|---|---|---|---|
| – | Трассировка лучей | `ray_tracing` (bool, false) | `--ray-tracing` | master switch |
| 1 | Отражения с трассировкой лучей | `rt_reflections` | `--rt-reflections` | ray-traced specular reflections |
| 2 | Освещение с трассировкой лучей | `rt_lighting` | `--rt-lighting` | ray-traced diffuse indirect light, one bounce (this design's reading of "lighting") |
| 3 | Тени с трассировкой лучей | `rt_shadows` | `--rt-shadows` | shadows of the dominant directional light |
| 4 | Локальные тени с трассировкой лучей | `rt_local_shadows` | `--rt-local-shadows` | shadows of point (and spot) lights |
| 5 | Глобальное затенение с трассировкой лучей | `rt_ambient_occlusion` | `--rt-ambient-occlusion` | ray-traced ambient occlusion (this design's reading of "глобальное затенение") |

- **Master switch.** Checked: every RT option whose level is not `off` runs. Unchecked: none runs. The five levels are kept as they are in both
  states (unchecking does not reset them). An option is *active* when the master switch is on and its level is not `off`.
- Local shadows (4) is a separate option (owner, 2026-10-19): the survey (`docs/spikes/S6-graphics-survey.md`) counts 5.01 M point-light channel
  uses against 2.79 M directional and 0 spot; the raster shadow map and contact shadows only follow a directional light.
- All RT options default to off, the master switch defaults to off (the original look stays the default, as for every other effect).

### RT replaces its raster counterpart (owner's decision)

While an RT option is active its raster counterpart does not run; the raster setting stays in `config.json` unchanged and comes back when the RT
option stops being active. In the launcher and the in-game menu the replaced raster option is greyed out with the reason "заменено трассировкой лучей"
(translated like the other strings).

| Active RT option | Raster effect that does not run |
|---|---|
| RT shadows (3) | shadow map (`shadows`), contact shadows (`contact_shadows`) |
| RT ambient occlusion (5) | GTAO (`ambient_occlusion`) |
| RT local shadows (4), RT reflections (1), RT lighting (2) | none (no raster counterpart exists; SSR was never built) |

Volumetric lighting is not built; if it lands later and wants a shadow map while RT shadows are active, that is its design's question.

### Denoising (owner's decision)

- **DLSS Ray Reconstruction** (Streamline `sl.dlss_d`, `kFeatureDLSS_RR`) when the upscaler is DLSS (any DLSS mode, DLAA included) on an NVIDIA RTX GPU
  that reports the feature. RR replaces the DLSS super-resolution evaluate for that frame (it upscales and denoises in one call); the own denoiser
  passes are skipped.
- **Own denoiser** everywhere else (FSR, SMAA, no upscaler, AMD/Intel, DLSS without RR support): temporal accumulation with reprojection by the
  existing motion vectors and depth/normal rejection, then a few iterations of an edge-aware à-trous spatial filter (SVGF-style, without
  variance-guided luminance for shadows/AO, with it for reflections/lighting). One instance per RT signal. No third-party denoiser library.
- RR is not a separate user option: it is used automatically when available (this design's call). `uc.exe --features` reports `ray_reconstruction`
  and its reason.

### Reflective surfaces (owner's decision)

GX has no material model. A surface reflects when:
1. its draw uses a GX lighting channel with the specular attenuation function (`ChannelLighting`, `scene_lights.h`); reflectivity follows the
   channel's material/specular colour; roughness is a fixed constant per preset table below, **or**
2. it is listed in a material list shipped with the game data (`dist/rt_materials.json`, this design's name and place), keyed by texture hash or
   guest callsite, giving reflectivity and roughness explicitly (floors, glass, water). A list entry overrides rule 1.

The list starts empty and is filled by tuning on reference frames. A debug switch (`UC_RT_DEBUG=reflective`) tints reflective pixels so the list can
be built from live captures.

## Goals

- The five options, the master switch, launcher widgets, in-game menu widgets, config load/save and CLI flags, live-switchable (no restart).
- Performance on the owner's RTX 4080, output 1440p with DLSS Quality (internal 960p; **this design's assumption, owner did not state a resolution**):

  | Setting | Target | RT GPU budget (all RT work: AS build, rays, denoise/RR, composite) |
  |---|---|---|
  | All five on High | ≈ 100 FPS (owner lowered it from 120 after phase 0, 2026-10-19) | ≤ 2.9 ms |
  | All five on Ultra | ≥ 60 FPS | ≤ 9.5 ms |
  | Low / Medium | RTX 3060 class (≈ 2.5x slower than the 4080) | Low ≤ 0.6 ms, Medium ≤ 1.2 ms total on the 4080 |

  Phase 0 (`docs/spikes/S8-ray-tracing.md`, fight3 demo) measured the RT-off frame at 7.11 ms (137 FPS), the AS build at 0.45 ms and the probe
  (one full-resolution shadow ray per pixel) at 1.0 ms GPU / 2.1 ms end to end; the measured internal resolution was 1707x1280, the 960p reference has
  about half the pixels, so the budgets are re-measured at the reference before presets are fixed.
  Budget split for High (this design's estimate, to be confirmed per effect): AS build 0.45, shadows 0.3, local shadows 0.4, AO 0.3,
  reflections 0.5, lighting 0.5, denoise 0.2 (RR replaces denoise and upscale; its cost replaces the DLSS SR cost and is counted separately).
  A preset that misses its budget gives up rays or resolution, never the target.
- **The FPS targets win over the ms budgets** (the owner stated them as "ideally"). The ms budgets are derived from the RT-off frame time (7.11 ms,
  phase 0); a later phase that finds a High preset cannot fit gives up rays or resolution first and reports to the owner with numbers before it lowers
  the target again (this design's call).
- "100 FPS" means rendered guest frames on `main`. The 240 Hz interpolation experiment (`experiment/fps-interpolation`) is not on `main` and is
  out of scope; if it lands later, RT runs on real frames only and interpolated frames reuse the composited result.
- Everything stays in the renderer: no guest code, guest timing or packet change; the guest never waits for RT work.
- With the master switch off (or every option off) the cost is nil: the deterministic bench (`python -m uctest bench … --demo fight2 --runs 3`) shows
  no loss above 1%, as for the raster effects.

## Non-goals

- Path tracing, multi-bounce GI, RT translucency/refraction, RT caustics.
- Replacing the game's lighting model: GX lighting stays the base; RT adds or removes light from it.
- Ray tracing on GPUs without hardware ray query support (no software fallback).
- Geometry the game did not draw this frame (see Constraints).

## Anti-goals (failure even if it works)

- Any RT effect applied to HUD, menus, movies or the 2D post chain.
- A frame rate loss with RT off.
- Gameplay freezes from pipeline or acceleration-structure work (AS builds happen on the GPU every frame; no CPU stalls waiting for them).
- Shimmer or ghosting the owner would notice in motion at High (denoise quality is part of done, not polish).
- Double shading: RT ambient occlusion on top of GTAO, RT shadows on top of the shadow map, RT lighting on top of the GX ambient term.

## Constraints from the game and the renderer

- **Only the frame's own geometry exists.** The scene is whatever the guest submits per frame. Objects behind the camera or culled by the game are
  missing from reflections and indirect light; off-screen occluders cast no shadow. The owner was told and did not object (2026-10-19).
- **Geometry arrives transformed per draw.** Vertices are decoded on the CPU (`renderer/gx_vertices.cpp`) with a position-matrix row per vertex;
  the VS applies the draw's matrix bank (`gx_transforms.*`), result in **view space**. Skinned and animated meshes change every frame. 98.4% of scene
  vertices come from one callsite (`80366E14`), all with normals.
- **View space.** GX lights are in view space per draw (`DrawState.lights`); the AS is built in the frame's view space too, so rays, lights and the
  G-buffer share one space and no camera matrix has to be recovered.
- **Only the main 3D scene.** Like every effect, RT applies to the draws before the scene boundary in a frame that has a scene
  (`SceneBoundary::frame_had_scene`). Other render passes (EFB copies to textures, the bloom workspace, previews) are neither traced nor captured
  as instances.
- **Effects path.** The scene targets (colour, depth, motion, G-buffer) at the scene boundary, as for the raster effects; RT needs them and
  `needs_scene_targets()` grows the RT condition.
- **8-bit colour.** The scene colour is RGBA8 and stays RGBA8 with RT (GX output is display-referred and already clamped; FSR, SMAA, the bloom
  chain and EFB copies all take 8-bit). RR runs with `colorBuffersHDR=false`.
- **Lights.** `scene_lights.*` collects the frame's GX lights (`CollectedLight`) and selects a stable dominant directional light (`select_dominant`).
  No spot lights were seen in the survey; the local-shadow path handles them like point lights with a cone check.
- **The existing shadow map** draws its casters inside the scene packets using last frame's dominant light (`vulkan_renderer.cpp`, casters at
  ~line 1742). The RT path does not reuse that map.

## How it renders (decided parts)

1. **Capture during the scene.** While scene packets are recorded, every scene mesh draw that writes depth and is a triangle list (the shadow-caster
   rule, alpha cut-outs included this time) is recorded as an RT instance: its vertex/index buffers, matrix bank, vertex count and a material record
   (TEV stage-0 texture, material colour, alpha-test reference, reflective flag/roughness, unlit/emissive class). Per-packet buffers are retained
   until the boundary submission completes, as the shadow design already does.
2. **At the boundary, AS build.** A compute pass applies each instance's matrix bank to its vertices into one per-frame position buffer (view
   space). One BLAS per instance (or a few merged BLAS per packet; builder's call), built with `PREFER_FAST_BUILD` every frame, no refit across
   frames (the guest gives no stable mesh identity). One TLAS, identity transforms. Alpha-tested instances are non-opaque; the ray-query candidate
   loop samples the stage-0 texture alpha against the alpha reference (approximation of the TEV alpha path; this design's call).
3. **Rays: inline ray queries in compute** (`VK_KHR_ray_query`), not ray-tracing pipelines (no shader binding table, fits the existing compute
   passes; this design's call). Hit shading for reflections and lighting reads the material record: stage-0 texture × material colour, lit by the
   frame's collected GX lights without shadow rays below Ultra, fog applied from the GX fog of the hit's distance. Texture access through descriptor
   indexing over the frame's scene textures.
4. **Per-pixel signals** at the internal resolution or the preset's fraction of it: sun visibility, local-light visibility, AO, reflection radiance,
   indirect irradiance. Each goes through the own denoiser, or, with RR, is composited noisy into the colour RR receives.
5. **Composite into the scene colour** before the upscale, faded by the G-buffer fog factor. RT shadows, AO and lighting apply only to pixels of
   the G-buffer class "GX-lit" whose draw has a single TEV light term the estimate can model (a "model-valid" bit in the material record, set when
   the lit colour channel feeds the TEV as a multiplier of a texture or constant); other pixels (unlit, emissive, baked colour, unmodelled TEV) are
   left untouched, so an error hides instead of darkening twice. Reflections apply on reflective pixels of any class.
   - Shadows (3, 4): the GX lighting equations are re-evaluated per pixel from the G-buffer (normal, position from depth) and the collected lights to
     estimate each light's share of the pixel's light; the visibility of each light scales its share; the result is one multiplier on the colour.
     (Exact per-light removal would need per-light terms written during raster; this estimate is this design's call.)
   - AO (5): multiplies the ambient share of the pixel (same estimate), not the whole colour.
   - Lighting (2): adds `albedo × indirect irradiance` and removes the GX ambient term it replaces (`albedo × channel ambient`), so ambient is not
     counted twice.
   - Reflections (1): blends the reflection radiance by a Fresnel term (Schlick, reflectivity from the material record) on reflective pixels.
6. **Albedo.** RT lighting, AO and RR need albedo. When any RT option is active the scene pass writes an extra target: stage-0 texture × material
   colour (no lighting), plus the channel ambient colour and the material record id. Builder decides the format and whether it joins the G-buffer
   attachment set or is a second attachment.
7. **RR inputs.** Noisy composited colour, depth, motion vectors, normals + roughness (from the G-buffer and material record), diffuse albedo,
   specular albedo (Fresnel-weighted reflectivity), optional specular hit distance from the reflection pass, camera matrices from the jitter/projection
   the DLSS path already passes.

## Presets (this design's call; tuned to the budget in phase 0)

Rates are rays per pixel at the stated resolution fraction of the internal resolution.

| Option | Low | Medium | High | Ultra |
|---|---|---|---|---|
| Shadows (3) | 1, ½ res, hard | 1, full, hard | 1, full, soft (sun cone) | 2, full, soft |
| Local shadows (4) | 1 light, ½ res | 2 lights, ½ res | 4 lights, full | 8 lights, full, soft (light radius) |
| AO (5) | 1, ½ res, short radius | 1, ½ res | 2, full | 4, full, longer radius |
| Reflections (1) | ½ res, roughness ≤ 0.3, no hit lighting beyond ambient | ½ res, ≤ 0.5 | full, ≤ 0.7 | full, ≤ 1.0, shadow rays at hits |
| Lighting (2) | 1, ¼ res | 1, ½ res | 1, ½ res, sky/fog fallback on miss | 2, full, shadow rays at hits |

Local-light selection per pixel: the N lights with the largest unshadowed contribution at that pixel among the frame's collected point/spot lights.

## Device requirements

`VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`, `bufferDeviceAddress`, descriptor indexing (runtime arrays of
sampled images), plus the effects path. Missing any: the RT group is greyed out with a reason (`--features` reports `ray_tracing`,
`ray_tracing_reason`, `ray_reconstruction`, `ray_reconstruction_reason`). Streamline gains `sl.dlss_d.dll` and `nvngx_dlssd.dll` from the same SDK
release (2.14.16) in `cmake/upscaler_sdks.cmake`, with their licence files.

## UI

- Launcher, Graphics page: a "Трассировка лучей" group under Effects: the master checkbox, then the five combos (enabled only while the master is
  checked; levels stay visible). In-game menu (Insert), Effects tab: the same group. Translations in `launcher/translations.inc` and the menu keys.
- Replaced raster options are greyed with the reason above.
- Options plumbing like every effect: `uc::Options`, `options_config.cpp`, `parse_options` + usage, `GraphicsEffects`, `LiveSettings`.

## Phases

0. **Spike (throwaway, measured):** AS build of a real fight frame (`fight2` demo) + one shadow ray per pixel. Output: AS build ms, instance and
   triangle counts per frame, ray cost, the RT-off frame time on the owner's settings. It also confirms that the Streamline 2.14.16 release ships
   `sl.dlss_d`/`nvngx_dlssd.dll` under a redistributable licence and that the existing DLSS path's jitter/projection inputs satisfy RR; if not, RR
   drops out and the own denoiser runs everywhere. Go/no-go per the Goals rule; re-split the budget table. Written to `docs/spikes/S8-ray-tracing.md`.
1. **Infrastructure:** device features and `--features`, options/config/CLI/UI with the master switch and raster replacement, instance capture,
   per-frame AS, albedo/material targets, timestamps, the own denoiser framework.
2. **RT shadows (3) and local shadows (4).**
3. **RT ambient occlusion (5).**
4. **DLSS Ray Reconstruction** (RR on the effects of phases 2–3 first, so its inputs are proven early).
5. **RT reflections (1)** with the material list and the debug tint.
6. **RT lighting (2).**

Each phase ends with host tests, a renderer smoke section, one `bench --demo fight2` (RT off: proves the off path is free) and one run with the
phase's options on (their cost), a live `uctest run fight` check, and is merged before the next. Visual sign-off is the owner's: each phase hands
over `uctest` captures of the same demo moments (`fight2` replay, fixed retraces) with the phase's options off and on at every preset, plus a short
motion capture for shimmer/ghosting.

## Testing

- Unit: options parse/load/save, master-switch and replacement logic (which raster passes run), preset tables, light-share estimate, denoiser math helpers.
- Renderer smoke: `renderer_effects_smoke --rt-*` sections on a fixture scene: an occluder darkens the receiver (shadows/local/AO), a reflective plane
  shows the object above it, indirect light brightens a wall facing a lit surface; all-off output identical to today's; `--validation` clean.
- Live: `uctest` captures with each option on for visual review; `demo check fight2` stays green with RT on (guest unaffected); frame log gets
  `gpu_rt_as_ms`, `gpu_rt_<effect>_ms`, `gpu_rt_denoise_ms`.

## Left to the builder

- Exact shaders, constants, sample patterns, denoiser iteration counts, as long as each preset meets its budget.
- BLAS granularity (per draw or merged per packet), build flags beyond fast-build, AS memory pooling.
- Format and attachment layout of the albedo/material target.
- Whether passes share dispatches (e.g. shadows + local shadows in one pass).

## Calls made in this design (owner to check)

- "Освещение" = one-bounce diffuse indirect light; "Глобальное затенение" = ray-traced AO.
- Output 1440p DLSS Quality is the reference for the 100/60 FPS goals.
- RR is automatic (no option); own denoiser elsewhere, no third-party library.
- Inline ray queries in compute; AS in view space, rebuilt every frame.
- Shadows/AO are applied through a per-pixel estimate of each light's share, not exact per-light terms.
- RT lighting removes the GX ambient term it replaces.
- No HDR: scene colour stays RGBA8, RR runs in LDR mode.
- RT shadows/AO/lighting skip pixels the light-share estimate cannot model.
- FPS targets win over ms budgets; phase 0 reports to the owner if the RT-off frame leaves too little room.
- RT applies to the main 3D scene only; the interpolation experiment is out of scope.
- Material list file `dist/rt_materials.json`, starts empty.
- Phase order (owner had no preference): shadows → AO → RR → reflections → lighting.
