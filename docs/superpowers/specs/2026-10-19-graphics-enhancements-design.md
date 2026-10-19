# Graphics enhancements (design)

Date: 2026-10-19. Status: draft for the owner's review.

## Intent

The port draws the game as the Wii did: GX per-vertex lighting (8 lights, `renderer/shaders/smoke.slang`), TEV colour combining
(`renderer/shaders/tev.slang`), GX fog, the game's own bloom chain, RGBA8 colour and D32 depth. This design adds eleven optional
improvements on top of that picture, each switchable on its own, so a player can keep the original look or build a remastered one:

1. Anisotropic filtering (up to 16x)
2. SMAA as an anti-aliasing choice next to DLSS and FSR
3. Per-pixel lighting
4. A normal G-buffer (MRT)
5. Ambient occlusion: GTAO
6. Contact shadows
7. Volumetric lighting and god rays
8. Per-object motion blur
9. Real shadows: shadow maps + PCSS
10. Screen-space reflections (SSR): **postponed by the owner (2026-10-19)**; the G-buffer keeps a reserved "reflective" flag.
11. Tessellation (PN triangles / Phong)

Ray tracing, path tracing and ray reconstruction are out of scope (later work).

## Quality presets and budgets

Owner: low/medium must run on weak hardware, high/ultra are for top GPUs of the owner's level (RTX 4080).
Budgets per effect, GPU time measured with the pass timestamps (numbers are this design's call):

| Preset | Reference | Output | Budget per effect |
|---|---|---|---|
| low | RTX 3060 / RX 6600 class | 1080p | ≤ 0.5 ms |
| medium | RTX 3060 / RX 6600 class | 1080p | ≤ 1.0 ms |
| high | RTX 4080 | 1440p | ≤ 1.0 ms |
| ultra | RTX 4080 | 4K (DLSS quality internal) | ≤ 2.0 ms |

Effects with presets: ambient occlusion, shadows, volumetric lighting (off, low, medium, high, ultra). Contact shadows, per-pixel lighting and
tessellation are on/off; motion blur is off/low/high (blur strength, same cost). The owner has no weak GPU: low/medium are checked on the RTX 4080
at the scaled budget (a 3060 is ~2.5x slower: low ≤ 0.2 ms, medium ≤ 0.4 ms on the 4080 at 1080p).

## Goals

- Every improvement is a separate graphics option in the launcher and in the in-game menu (Insert), saved in `config.json`, with a CLI flag for tests.
  Exception: the G-buffer (4) has no visible effect of its own; it turns on whenever an option that reads it is on (decision of this design).
- No cost on the game's own threads. The guest runs on the scheduler's host threads (`GuestRunner` in `game/uc_main.cpp`); the renderer runs on the
  main host thread and consumes `gx::FramePacket`s from a queue. All new work lives in the renderer (GPU passes, render-thread recording, renderer
  worker threads) and never adds work, locks or waits to the guest side. The guest never reads EFB pixels back (every copy is `gpu_only`), so no
  effect can change guest behaviour or determinism.
- The extra work goes to idle resources: GPU passes at reduced resolution where the result allows it, compute where possible, async compute where a
  pass is independent of the graphics queue, and command recording on renderer worker threads where recording cost is measurable.
- Every effect is off-by-default except anisotropic filtering (decision of this design: the original look stays the default; anisotropy only sharpens textures).
- All off with anisotropy 1 = the current picture, pixel for pixel, on the current path (the effects path is not required to be bit-exact against it).
- Effects apply only to the main 3D scene: the draws before the scene boundary in a frame that has a scene (`SceneBoundary::frame_had_scene`).
  Menus, movies, black frames, the post chain, the HUD and any perspective draw after the boundary stay untouched.

## Non-goals

- No change to guest code, guest timing or the packet stream.
- No HDR pipeline in this design (RGBA8 stays; see Constraints).
- No texture packs, colour grading, DOF, film grain (proposed earlier; separate work).
- No ray tracing.

## Anti-goals (failure even if it "works")

- Any effect that lowers the guest-bound FPS of the deterministic bench when the effect is OFF (the off path must cost nothing measurable, > 1% with `--runs 3`).
- Gameplay freezes from pipeline compiles (pipeline warm-up design: no gameplay freezes; a pre-start compile screen is acceptable).
- HUD, menus, movies or the 2D post chain affected by a 3D effect.
- Effects that break DLSS/FSR inputs (AO or shadow shimmer that the upscaler then smears, motion vectors altered).

## Constraints from the current renderer

- **Scene targets exist only with an upscaler.** `scene_color_`/`scene_depth_`/`scene_motion_` and the scene split at `gx::SceneBoundary` are created
  by `rebuild()` when an upscaler object exists (DLSS/FSR, test blit). Screen-space effects need the split even with the upscaler off or SMAA. The design
  generalises it: the scene-target path runs when any upscaler/AA or any screen-space effect is on. With no upscaler, the existing blit upscaler
  (`make_blit_upscaler`, ratio 1) becomes the production pass-through.
- **Pipeline sets.** On GPUs with unused-attachment support the motion pipeline set is already used for every draw, with the motion attachment
  bound only in scene passes and left empty otherwise. The G-buffer joins that set the same way, as a third attachment bound only when an
  effect reads it (planning finding, 2026-10-19; it replaces the separate "enhanced" set the first draft had). No restart is needed: every toggle is a
  push-constant flag (per-pixel lighting), a bound/unbound attachment (G-buffer) or a pass that runs or not. The cost with effects off (an empty
  attachment, four more interpolants) is checked by the bench against the 1% anti-goal. Only tessellation (11) and the shadow casters (9) add pipelines.
- **GPU requirement.** The effects path needs what the motion path needs today (`VK_EXT_dynamic_rendering_unused_attachments` + `independentBlend`).
  Without it options 3–11 are greyed out with a reason; 1 and 2 still work.
- **Lighting data.** GX light positions and directions are in view space per draw (`DrawState.lights`, `channels`, `ambient`, `materials`). The VS
  has the view-space normal; the FS does not receive it today.
- **8-bit colour.** TEV results and EFB copies are 8-bit. Effects darken or add to the scene colour before the scene boundary; they do not need HDR.
- **Fog.** GX fog is applied in the FS. AO/shadows/contact shadows must fade with the fog amount, so the G-buffer carries the per-pixel fog factor.
- **The game's own content** (to be measured in phase 0): which scene draws use GX lighting channels versus baked vertex colours, light kinds
  (directional/point/spot) and counts, whether the game draws its own blob or projected shadows, which draws are opaque, alpha-tested, blended.

## Frame structure with effects on

Per guest frame, in the existing render order:

The renderer handles one packet at a time and learns where the scene ends only when the boundary packet arrives, so nothing waits for a whole
frame. Everything that needs the whole scene runs at the boundary:

1. **Scene raster** into the scene targets: colour, depth, motion (when vectors are needed: upscaler or motion blur), and the G-buffer.
   Per-pixel lighting (3) and tessellation (11) happen here. While the scene packets are recorded the renderer keeps a list of the scene's shadow
   casters (mesh, palette, transforms, alpha-test state) and of the GX lights the scene draws used, and retains the per-packet buffers they need
   until the boundary submission completes.
2. **At the boundary, before the upscale:** light selection (9, 6, 7) from that list; the shadow pass (9) re-draws the listed casters depth-only
   from the chosen light (same VS, light view-projection), recorded on renderer worker threads into secondary command buffers when recording
   measures above 0.3 ms; then the scene effects in compute on the scene targets: occlusion (GTAO 5 + contact shadows 6 + the deferred shadow
   mask 9) → multiplied into colour; volumetric lighting (7) → composited. Reduced-resolution passes upsample bilaterally by depth.
   The game's own scene copy (its bloom input) is taken after this, so bloom sees the effects.
3. **Upscale / AA**: DLSS, FSR, SMAA (2) or the pass-through blit.
4. **Motion blur** (8) at output resolution, after the upscale and before the game's post chain (bloom) and the HUD.
5. Post chain and HUD as today.

## Per-feature design

### 1. Anisotropic filtering
- Option: `graphics.anisotropy` = 1, 2, 4, 8, 16 (default 16); launcher combo, menu combo, `--anisotropy N`.
- Enable `samplerAnisotropy` when the device has it; clamp to `maxSamplerAnisotropy`.
- Applies only to samplers with linear min filtering and mipmaps, and only to scene draws (`scene_samplers_` and the no-upscaler equivalent). HUD,
  nearest-filtered and single-mip textures keep their samplers unchanged. Changing it recreates samplers at a frame boundary (no device change).
- Works without the scene-target path.

### 2. SMAA
- `smaa` becomes a value of `graphics.upscaler` (`off`, `smaa`, `dlss`, `fsr`); the UI label becomes "Anti-aliasing / upscaling". It is an
  `Upscaler` at ratio 1: SMAA 1x (edge detection on luma, blend weights, neighbourhood blending) as three compute or fullscreen passes, preset High.
- No jitter, no motion vectors needed; quality/frame-generation settings are hidden for SMAA.
- Shader source: the reference SMAA (MIT licence) with its area/search textures, licence noted in `THIRD_PARTY_NOTICES.md`.

### 3. Per-pixel lighting
- Option: `graphics.per_pixel_lighting` (bool).
- The VS passes view position and view normal; the FS evaluates the same GX channel equations (`lightControl`) per pixel for channels that use
  lighting, and keeps the vertex result for channels that do not (baked colour). One shader, a per-frame push-constant bit: live toggle.
- The GX equations stay exact (same attenuation, spot, diffuse functions); only the evaluation point moves. No new specular model: GX specular
  channels (attenuation function "spec") already exist and become per-pixel too.
- Risk: models whose artists relied on vertex interpolation look harsher; the option stays off by default.

### 4. G-buffer (MRT)
- One extra colour attachment, `VK_FORMAT_R16G16B16A16_UNORM`: octahedral view normal (2×16 bit), fog factor (16 bit), material flags (16 bit field:
  has-normal, GX-lit, baked colour, emissive/unlit, tessellated, reflective; the rest reserved).
- Written by every scene draw of the enhanced pipeline set; the write mask is off for blended draws (decals, particles) so they keep the
  surface below.
- Normal source: the interpolated view normal, or depth-derived normals for draws without one.
- Tests/tools: `capture_normals()` like `capture_motion()`.

### 5. GTAO
- Option: `graphics.ambient_occlusion` = off, low, medium, high, ultra (XeGTAO's low/medium/high/ultra slice and step counts; low/medium at half
  resolution, high/ultra at full).
- XeGTAO-style compute: horizon search on depth + G-buffer normals, spatial denoise. Temporal accumulation is the upscaler's (DLSS/FSR); with SMAA
  or no upscaler the noise pattern is fixed per pixel (no frame-to-frame shimmer).
- Applied by multiplying scene colour, faded by the fog factor and skipped on unlit/emissive classes. Radius and strength are fixed constants tuned
  once on reference frames.

### 6. Contact shadows
- Option: `graphics.contact_shadows` (bool).
- Short screen-space ray march toward the frame's dominant light (from the light selection of 9, or the strongest GX light of the scene draws when
  shadows are off). Half resolution, bilateral upsample, applied in the same occlusion composite as AO.

### 7. Volumetric lighting and god rays
- Option: `graphics.volumetric_lighting` = off, low, medium, high, ultra.
- Low/medium: screen-space light shafts (radial blur of the bright, non-occluded pixels toward the projected light position) for the dominant
  light, quarter / half resolution.
- High/ultra: froxel volume (160×90×64 / 240×135×128) in view space, lit by the frame's GX lights (deduplicated across draws), with the shadow map when shadows are
  on, integrated front to back and composited with the scene by depth. Density follows the frame's GX fog colour/range so it matches the
  game's atmosphere. Compute; candidate for async compute.

### 8. Per-object motion blur
- Option: `graphics.motion_blur` = off, low, high (shutter fraction).
- McGuire-style tile max / neighbour max reconstruction from the motion vectors and depth, at output resolution after the upscale, before bloom.
- Needs motion vectors: motion outputs are on whenever motion blur is on, even with SMAA or no upscaler (jitter stays zero then).
- Camera cuts (`tracker_.camera_cut()`) and black frames skip the pass.
- With frame generation the blur stays on the rendered frames; the generator gets the blurred HUD-less frame and the unchanged motion vectors.

### 9. Shadow maps + PCSS
- Option: `graphics.shadows` = off, low (1024², 4-tap PCF), medium (2048², PCF 3×3), high (2048², PCSS), ultra (4096², PCSS, more blocker/filter taps).
- Light selection per frame: one shadow-casting light, the directional or spot light that lights the most scene draws (by count of lit draws and
  colour intensity), stable across frames (hysteresis) so shadows do not jump. Point lights do not cast shadows in this design.
- Casters: the opaque, depth-writing scene draws listed during the scene. Opaque casters without alpha test share one depth-only pipeline per
  vertex layout (no FS); alpha-tested casters use one ubershader-based depth pipeline that runs the TEV alpha path (no per-key variants, no warm-up growth).
- Receivers: every scene pixel through the deferred shadow mask (depth reprojected into the light, PCF/PCSS), multiplied into colour in the
  occlusion composite with a fixed strength per G-buffer class (GX-lit, baked colour; emissive/unlit pixels are skipped), faded by the fog factor.
  Shadows do not require per-pixel lighting.
- Fit: the light frustum covers the camera frustum up to a fixed distance; one cascade for spot lights, two cascades for a directional light.
- If phase 0 finds the game's own shadows, they are suppressed by callsite when shadows are on (like `gx::bloom_composite`).

### 10. SSR
- Postponed by the owner. Nothing is built; the reserved G-buffer flag is the only trace.

### 11. Tessellation
- Option: `graphics.tessellation` (bool).
- Hardware tessellation (tessellation control/evaluation stages) for lit, triangle, opaque mesh draws that carry normals; PN triangles with a
  fixed factor that falls off with distance. Interpolation happens in view space after skinning, so per-vertex matrix indices never split a patch.
- A new pipeline variant per eligible TEV key, compiled in the warm-up; draws whose tessellated pipeline is not ready use the normal one.
- Risk: cracks where two draws share an edge with different normals; phase 0 measures how many eligible meshes exist and the result decides whether
  the option ships.

## Options, UI and plumbing (all features)

- `uc::Options` field + `options_config.cpp` load/save (`graphics.*`) + CLI flag in `parse_options` + usage text.
- `LiveSettings`: a `graphics_effects` target that hands the whole effects block to the renderer (`VulkanRenderer::set_effects(const GraphicsEffects&)`),
  applied at a guest frame boundary; switching the scene-target path on or off goes through `rebuild()` like an upscaler change.
- Launcher: a new "Effects" group on the graphics page under the upscaling block, same widgets (`combo`, `toggle`), translations in
  `launcher/translations.inc`. In-game menu: a third tab "Effects" in `SettingsMenu`, same layout and translation keys.
- Device capability: options the GPU cannot run (no tessellation shader, no anisotropy) are greyed out with a reason, via the existing
  `--features` JSON report.

## Performance method

- Each effect gets GPU timestamps around its passes (extending `timing_pool_`), reported in the frame log and `--profile`.
- Each feature lands with one `python -m uctest bench <before> <after> --demo fight2` run (effect off in both: proves the off path is free) and one
  run with the effect on (its cost). Live FPS check with `uctest run fight` once per feature.
- CPU: the render thread time per frame (`render_ns`) must not grow with effects off; shadow-pass recording moves to worker threads if it shows up.
- Async compute: a compute-capable queue (separate family when the GPU has one) for passes 3/7 that do not depend on the graphics work in flight;
  adopted only if the bench shows a gain.

## Phases

0. **Survey (offline, packet dumps):** lighting channel usage per scene draw, light kinds and counts, game shadows, opaque/alpha mix, mesh normals,
   candidate reflective materials. Output: `docs/spikes/S6-graphics-survey.md`. Decides 9/10/11 details.
1. **Quick wins:** 1 (anisotropy), 2 (SMAA), the effects block and its UI group/tab; each later feature adds its own option when it lands
   (no option is shown before its effect works).
2. **Scene-target path without an upscaler, G-buffer, per-pixel lighting:** 4, 3.
3. **Screen-space occlusion:** 5, 6.
4. **Shadows:** 9 (+ contact shadows use its light).
5. **Volumetrics, motion blur:** 7, 8.
6. **Tessellation:** 11.

Phases 0–3 are planned in detail now; phases 4–6 get their detailed plan after the survey (owner, 2026-10-19).

Each phase ends with tests, a bench and a live check, and is merged before the next.

## Testing

- Unit: option parse/load/save round trips, light selection, effect-block diffing (restart needed or live), SMAA/AO/shadow math helpers.
- Renderer smoke (`tests/renderer_*`): a fixture scene per effect with a pixel check (AO darkens a crease, shadow darkens the receiver, SMAA
  reduces an edge's step count, all-off output identical to today's).
- Live: uctest scenarios capture frames with each effect on for visual review; `demo check fight2` stays green with effects on (guest unaffected).

## Left to the builder

- Exact shader sources and constants (GTAO radius, PCSS kernel), as long as each preset stays within its budget in the quality table.
- Whether a pass is compute or fullscreen graphics.
- Recording structure (secondary command buffers or not) where the measurement does not ask for it.

## Decisions made in this design (owner to confirm)

- Anisotropy defaults to 16x; every other effect defaults to off.
- G-buffer has no option of its own.
- SMAA is a value of the existing upscaler setting.
- Shadows are applied as a deferred mask on every surface; one shadow-casting light per frame; point lights cast no shadows.
- The G-buffer is a third attachment of the existing motion pipeline set; every effect toggle is live (scene targets are created or dropped by
  `rebuild()`, as an upscaler change does today).
- Motion blur stays on with frame generation; the game's bloom sees the effects.
- No HDR pipeline.
