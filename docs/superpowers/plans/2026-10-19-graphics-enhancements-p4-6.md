# Graphics enhancements, phases 4–6 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add shadow maps with PCF/PCSS, per-object motion blur, volumetric light (screen-space shafts, then a froxel volume) and PN-triangle tessellation, each as its own launcher and in-game option.

**Architecture:** Same as phases 0–3 (`docs/graphics-effects.md`): GPU passes recorded on the render thread, nothing on guest threads, every toggle live once the scene-target path exists. New in this plan: the shadow map is drawn **while the scene packets are recorded**, with the previous frame's dominant light (see Rulings), so no draw has to be retained until the scene boundary.

**Tech Stack:** C++20, Vulkan 1.3 dynamic rendering + compute, Slang → SPIR-V, existing `OcclusionPass` / `PassTimer` / `GraphicsEffects` plumbing.

**Spec:** `docs/superpowers/specs/2026-10-19-graphics-enhancements-design.md`. Survey: `docs/spikes/S6-graphics-survey.md`.

## Global Constraints

- Everything from the phases 0–3 plan holds: no guest-side work, all-off equals the old picture, effects touch scene draws only, defaults off (anisotropy 16), budgets per effect: low ≤ 0.5 ms (RTX 3060, 1080p), medium ≤ 1.0 ms (same), high ≤ 1.0 ms (RTX 4080, 1440p), ultra ≤ 2.0 ms (RTX 4080, 4K). The owner's RTX 4080 at 1080p checks low ≤ 0.2 ms and medium ≤ 0.4 ms.
- 13-language strings in `launcher/translations.inc`; option plumbing as in phases 0–3 (`GraphicsEffects`, `options.cpp`, `options_config.cpp`, `settings_menu.cpp`, `launcher/main.cpp`, tests `test_options`, `test_options_config`, `test_live_settings`, `test_settings_menu`).
- Source edits with Edit/Write only. Build with the scratchpad `bt.bat` pattern or `build.bat --no-install`; Python 3.13 for `uctest bench/demo` (`py -3.13`).
- Survey facts (S6): 98% of scene vertices come from one mesh callsite (`80366E14`); lights in the game are directional + point (no spot); no game-shadow callsite exists; 84% of those draws are depth-writing; 100% of mesh vertices carry normals.
- Commit only on the owner's go-ahead per task group (the owner had the executor commit per task in phases 0–3).

## Rulings of this plan (differences from the spec, with reasons)

1. **Shadow map drawn per scene packet, not at the boundary.** The scene arrives as many packets, each submitted on its own; retaining their palette/uniform buffers until the boundary would need per-slot buffers to outlive their fences. Drawing the casters into the shadow map inside each scene packet's command buffer needs no retention. Cost: the light is last frame's `dominant_light_` (stable by the 25% hysteresis), and the first frame after a scene starts has no shadows.
2. **View-space shadow fit.** All palettes and lights are already in view space, so the light's orthographic box is built in view space from the camera frustum up to `kShadowDistance` (30 view units, a constant tuned on the train car) and the light direction; no world matrices are needed.
3. **Casters are mesh draws only** (98% of vertices, no per-draw CPU vertex copies). Non-mesh draws cast no shadow.

## Review Focus

1. **No dominant light (point lights only, or none).** Expect no shadow pass, no mask, an unchanged picture. Task 14 test.
2. **Camera cut / scene to menu while shadows are on.** Expect the shadow map and mask to be ignored on a frame without a scene and rebuilt on the next. Task 15 smoke.
3. **Light switches between frames.** Expect no flicker beyond one frame and no stale map applied with the new light. Task 14 (`light_changed` resets) and Task 15.
4. **Motion blur with a camera cut or black frame.** Expect no smear. Task 17.
5. **Tessellation next to non-tessellated neighbours.** Expect no cracks above the measured limit, or the option does not ship. Task 21 gate.

---

## Phase 4 — Shadows

### Task 14: Shadow fit and PCSS math

**Files:** Create `renderer/shadow_math.h`, `tests/test_shadow_math.cpp`; register in `tests/CMakeLists.txt`.

**Interfaces:**
- Consumes: `uc::ViewParams` (`renderer/view_reconstruct.h`), `gx::CollectedLight` (`renderer/scene_lights.h`).
- Produces (all in `uc`):
  - `struct ShadowFit { std::array<std::array<float,4>,4> view_to_light; float extent; float depth_range; float texel_world; };`
  - `std::optional<ShadowFit> fit_shadow(const gx::Projection& scene_projection, const gx::CollectedLight& light, float distance, uint32_t resolution);` — null for a degenerate light direction. The box covers the frustum slice 0..`distance`, is centred on it, and its centre snaps to whole texels (no shimmer when the camera moves).
  - `struct ShadowPreset { uint32_t resolution; uint32_t filter_taps; uint32_t blocker_taps; bool soft; };` and `ShadowPreset shadow_preset(EffectLevel)` — low 1024² 4 taps, medium 2048² PCF 3×3, high 2048² PCSS, ultra 4096² PCSS with more taps (spec table).
  - `bool light_changed(const std::optional<gx::CollectedLight>& a, const std::optional<gx::CollectedLight>& b);` — true when kind or direction differ by more than 1e-3.

- [ ] **Step 1: Write the failing test** `tests/test_shadow_math.cpp`:
  - `fit_shadow` of a 60° 16:9 projection with a light straight down the view axis: a view-space point at `{0,0,-10}` maps into `[-1,1]²` and depth `[0,1]`; a point at `{0,0,-40}` (beyond `distance` 30) maps outside depth `[0,1]`.
  - Moving the camera frustum by less than one texel does not change `view_to_light` translation (snapping); by one texel does.
  - `fit_shadow` with `light.position={0,0,0}` and `kind=directional` returns `nullopt`.
  - `shadow_preset(low).resolution==1024`, `ultra.resolution==4096 && soft`, `off` is a zero resolution.
  - `light_changed` is false for equal lights and for a 1e-4 difference, true for a changed direction.
- [ ] **Step 2: Run:** `ctest --test-dir build -R test_shadow_math --output-on-failure`. Expected: FAIL (does not compile).
- [ ] **Step 3: Implement** `renderer/shadow_math.h` (header only): an orthonormal light basis from the normalised direction, the frustum slice corners from `view_params`, their bounds in light space, texel snapping of the box centre.
- [ ] **Step 4: Run** the same command. Expected: PASS.
- [ ] **Step 5: Commit.** "Add shadow fitting and presets".

### Task 15: Shadow map pass inside the scene packets

**Files:** Create `renderer/shadow_pass.h/.cpp`, `renderer/shaders/shadow.slang` (vertex-only variant of `smoke.slang`'s vertex stage); modify `renderer/vulkan_renderer.h/.cpp` (record in `record_pass` for scene packets), `renderer/CMakeLists.txt`, `renderer/graphics_effects.h` (`EffectLevel shadows`), `tests/renderer_effects_smoke.cpp` (`--shadows` section).

**Interfaces:**
- Consumes: `ShadowFit`, `shadow_preset` (Task 14); `dominant_light_` of the previous frame; the draw's palette/descriptor set already bound by `record_pass`.
- Produces:
  - `class uc::ShadowPass { void initialize(VkDevice, const VkPhysicalDeviceMemoryProperties&, const std::filesystem::path& shaders); void begin_frame(VkCommandBuffer, const ShadowFit&, uint32_t resolution); bool record_draw(VkCommandBuffer, const DrawGpu-like inputs...); void end_frame(VkCommandBuffer); VkImageView map_view() const; ~...}` with a D32 image of the preset size, cleared to 1 at `begin_frame` (once per guest frame, in the first scene packet), loaded afterwards.
  - Only mesh draws with depth write and no blending, culling off, a constant depth bias and slope bias. `GraphicsEffects::shadows` (default `off`); `needs_scene_targets` and `effects_need_lights` include it.

- [ ] **Step 1: Write the failing smoke section** `--shadows` in `renderer_effects_smoke.cpp`: the contact-shadow fixture (box on a floor, directional light) with the shadow level `medium`: floor pixels right of the box's base are ≥ 25 levels darker than with shadows off; floor far from the box unchanged (≤ 1); a point-light-only scene is identical to off (Review Focus 1); a frame without a scene records no shadow pass (`pass_runs(GpuPass::shadow)` unchanged); switching the level 20 times creates no per-frame resources; `--validation` clean.
- [ ] **Step 2: Run:** `renderer_effects_smoke.exe --shadows --validation`. Expected: FAIL.
- [ ] **Step 3: Implement** the pass: depth-only pipeline (specialised only on topology and the palette layout; alpha-tested casters are skipped in this task and listed as a limit), the shadow draws appended in `record_pass` for scene packets after the main draws are recorded in the same command buffer (a separate dynamic rendering block), `GpuPass::shadow` timer, the map recreated on a level change after `present_.wait_idle`.
- [ ] **Step 4: Run** the section. Expected: map exists and is drawn, no mask yet; test still fails on the darkening check.

### Task 16: Deferred shadow mask with PCF and PCSS

**Files:** Modify `renderer/shaders/gtao.slang` (a shadow term in `compositeMain`), `renderer/occlusion_pass.h/.cpp` (shadow map binding, `ShadowFit` push constants), `renderer/vulkan_renderer.cpp`.

**Interfaces:**
- Consumes: Task 14 and 15 outputs, the G-buffer flags (`kGBufferLit`).
- Produces: `OcclusionInputs::shadow` (`std::optional<ShadowInputs{VkImageView map; ShadowFit fit; ShadowPreset preset;}>`). The composite reprojects each surface pixel into the light, applies low/medium PCF or high/ultra PCSS (blocker search then penumbra-sized PCF), multiplies colour by `lerp(1, visibility, 0.55 * (1 - fog))` on `HasNormal` texels and skips unlit/emissive pixels.

- [ ] **Step 1:** the Task 15 smoke section is the test (darkening ≥ 25, far floor ≤ 1, point-light scene identical).
- [ ] **Step 2: Run, expect FAIL** on darkening. **Step 3: Implement** the shader code and bindings. **Step 4: Run, expect PASS** with `--validation`.
- [ ] **Step 5: Options and UI:** `graphics.shadows` (off/low/medium/high/ultra), `--shadows`, menu combo and launcher combo (translation key `shadows`), tests in the four option tests. Run `ctest`, expect all pass.
- [ ] **Step 6: Budgets and look:** `python -m uctest run fight --game-arg=--shadows --game-arg=high --game-arg=--profile` shows `[gpu] passes:` with the shadow time; expect medium ≤ 0.4 ms and high ≤ 1.0 ms on the RTX 4080 at the harness size, two captured frames reviewed by the owner. One `bench` round with shadows off confirms the off path.
- [ ] **Step 7: Commit.** "Add shadow maps with PCF and PCSS".

## Phase 5 — Motion blur and volumetric light

### Task 17: Per-object motion blur

**Files:** Create `renderer/motion_blur_pass.h/.cpp`, `renderer/shaders/motion_blur.slang`; modify `renderer/graphics_effects.h` (`EffectLevel motion_blur`), `vulkan_renderer.cpp` (run after `upscaler_->evaluate`, before the post chain; motion outputs on whenever the effect is on), `game/*` and `launcher/*` plumbing, `GpuPass::motion_blur`.

**Interfaces:**
- Consumes: `scene_motion_` vectors (RG16F, internal pixels), the upscaled colour in `efb_`, `tracker_.camera_cut()`.
- Produces: compute passes tile-max → neighbour-max → gather (McGuire), shutter fraction low 0.3 / medium 0.5 / high 0.7 / ultra 0.7 with more samples; vectors scaled to output pixels; skipped on camera cuts and black frames.

- [ ] **Step 1: Write the failing smoke section** `--blur`: a quad moved 40 px between frames with vectors on; the blurred edge has ≥ 8 distinct intermediate values along the motion axis versus ≤ 2 off; a still scene is identical to off; a camera cut frame is identical to off (Review Focus 4).
- [ ] **Step 2–4:** run (FAIL), implement, run (PASS, `--validation`).
- [ ] **Step 5:** options/UI/translations (`motion_blur`), the four option tests, budgets (low ≤ 0.2 ms at 1080p on the 4080), live fight capture. **Step 6: Commit.**

### Task 18: Volumetric light, screen-space shafts (low, medium)

**Files:** Create `renderer/shaders/volumetric_shafts.slang`; extend `OcclusionPass` or add `renderer/volumetric_pass.h/.cpp`; `EffectLevel volumetric_lighting`; plumbing as above; `GpuPass::volumetric`.

**Interfaces:**
- Consumes: `dominant_light_` (directional: shafts toward its projected direction; the spot/directional position projected with the scene projection), scene depth, scene colour, G-buffer fog.
- Produces: radial blur of the bright, unoccluded pixels toward the projected light, quarter (low) or half (medium) resolution, composited additively with the fog colour tint; nothing when the light is behind the camera or off screen beyond a margin.

- [ ] **Step 1: Failing smoke `--shafts`:** a bright strip behind an occluder, light projected at the strip: pixels between occluder and light brighten ≥ 10 levels; no light → identical to off; light behind the camera → identical to off.
- [ ] **Step 2–4:** run, implement, run. **Step 5:** options/UI/budgets (low ≤ 0.3 ms, medium ≤ 0.6 ms on the 4080 at 1080p). **Step 6: Commit.**

### Task 19: Volumetric light, froxel volume (high, ultra)

**Files:** extend the volumetric pass; `renderer/shaders/volumetric_froxel.slang`.

**Interfaces:**
- Consumes: shadow map and `ShadowFit` when shadows are on (else unshadowed), the scene's GX lights (`SceneLightCollector::lights()`, up to 8), GX fog colour and range from the draw uniforms (carry them through `SceneLightCollector` or a fog snapshot of the first scene draw).
- Produces: a 160×90×64 (high) / 240×135×128 (ultra) view-space volume, in-scatter integrated front to back, composited by depth. Budget high ≤ 1.0 ms at 1440p, ultra ≤ 2.0 ms at 4K on the 4080.

- [ ] **Step 1: Failing smoke `--froxel`:** a directional light and a shadow-casting box in a uniformly fogged scene: pixels in the shadow volume stay darker than lit-volume pixels by ≥ 8 levels; shadows off → both equal. **Step 2–4:** run, implement, run. **Step 5:** presets in the existing `volumetric_lighting` option (low/medium → Task 18 path, high/ultra → froxels). **Step 6:** live capture, owner review, commit.

## Phase 6 — Tessellation

### Task 20: Survey for tessellation candidates

**Files:** `docs/spikes/S7-tessellation-survey.md`; extend `UC_LIGHT_STATS` (or a new `UC_MESH_STATS`) in `game/packet_dump.h`.

- [ ] **Step 1:** add per-mesh stats: distinct meshes, triangles, shared-position edges whose normals differ by more than 15° (potential cracks).
- [ ] **Step 2:** run train and fight scenarios with the stats; record counts.
- [ ] **Step 3:** write the survey with a go/no-go: go when ≥ 60% of scene triangles belong to meshes whose shared edges have matching normals (< 15°) or are duplicated-vertex-free; otherwise the option is dropped and this phase ends. **Commit.**

### Task 21: PN-triangle tessellation (only after a go in Task 20)

**Files:** `renderer/shaders/smoke.slang` (tessellation control and evaluation entry points for the MOTION variant), `renderer/vulkan_renderer.cpp` (pipeline variant with `VK_PRIMITIVE_TOPOLOGY_PATCH_LIST`, `tessellationShader` feature), `pipeline_keys.h` (a key bit), warm-up key files, `GraphicsEffects::tessellation`, plumbing, `FeatureReport` field `tessellation`.

- [ ] **Step 1: Failing smoke `--tess`:** a low-poly sphere with normals: silhouette pixel count differs from the untessellated render by ≥ 3%; a flat quad is identical; draws whose tessellated pipeline is not compiled yet use the normal pipeline (no stall); `--validation` clean.
- [ ] **Step 2–4:** run, implement (PN triangles in view space after skinning, factor falling off with distance from 1 to 8), run.
- [ ] **Step 5:** crack check on a demo replay with captures at fixed retraces; limit set from Task 20. **Step 6:** UI, capability grey-out, budgets, commit.

## After this plan

Re-check the spec's "Decisions made" list with the owner (anisotropy default 16, no HDR) and consider SSR again (G-buffer flag `kGBufferReflective` is reserved).
