# Ray tracing, phases 0–1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure whether ray tracing fits the owner's budget on a real fight frame (phase 0), then build the shared RT infrastructure every RT effect needs: device support, per-frame acceleration structures, the RT options model with the master switch and raster replacement, the albedo/material target and the own denoiser (phase 1).

**Architecture:** Same as the raster effects (`docs/graphics-effects.md`): GPU work recorded on the render thread, nothing on guest threads. Bottom-level acceleration structures are built **inside each scene packet's command buffer** (as the shadow map is drawn today), the top-level structure at the scene boundary; rays are inline ray queries in compute passes on the scene targets.

**Tech Stack:** C++20, Vulkan 1.3 + `VK_KHR_acceleration_structure` + `VK_KHR_ray_query`, Slang → SPIR-V, existing `PassTimer` / `GraphicsEffects` / `LiveSettings` / `FeatureReport` plumbing, Streamline 2.14.16 (already fetched; `sl.dlss_d.dll`, `nvngx_dlssd.dll`, `include/sl_dlss_d.h` are in `build/_deps/streamline-src`).

**Spec:** `docs/superpowers/specs/2026-10-19-ray-tracing-design.md`. Survey: `docs/spikes/S6-graphics-survey.md`. Raster effects plan for the plumbing pattern: `docs/superpowers/plans/2026-10-19-graphics-enhancements-p4-6.md`.

**Scope:** phases 2–6 (shadows, local shadows, AO, Ray Reconstruction, reflections, lighting) get their detailed plan after Task 3's measurements, as the raster effects did after their survey. Phase 0's numbers can change the presets and budgets those phases inherit.

## Global Constraints

- No guest-side work, no packet change; RT applies only to draws before the scene boundary in a frame with a scene (`SceneBoundary::frame_had_scene`).
- RT off (master off or every RT level off) costs nothing: `python -m uctest bench <before> <after> --demo fight2 --runs 3` within 1%, `render_ns` unchanged.
- Targets (owner): all five RT options on High ≈ 120 FPS, all on Ultra ≥ 60 FPS, RTX 4080, reference output 1440p DLSS Quality. FPS targets win over the ms budgets (High ≤ 3.5 ms all RT work, Ultra ≤ 10 ms, Low ≤ 0.6 ms, Medium ≤ 1.2 ms on the 4080).
- Scene colour stays RGBA8; no HDR target.
- Device requirements: `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`, `bufferDeviceAddress`, descriptor indexing, plus the effects path. Missing any: RT unavailable with a reason, never a crash, the rest of the renderer unchanged.
- Config names: `graphics.ray_tracing` (bool), `graphics.rt_reflections`, `graphics.rt_lighting`, `graphics.rt_shadows`, `graphics.rt_local_shadows`, `graphics.rt_ambient_occlusion` (off/low/medium/high/ultra); CLI `--ray-tracing`, `--rt-reflections`, `--rt-lighting`, `--rt-shadows`, `--rt-local-shadows`, `--rt-ambient-occlusion`. All default off.
- No option is shown in the launcher or the in-game menu before its effect works (rule of the raster effects); the UI group arrives with phase 2.
- Source edits with Edit/Write only. Build with `build.bat --no-install`; tests `ctest --test-dir build --output-on-failure`; GPU smoke tests from `build/renderer` (`VK_LOADER_LAYERS_DISABLE=~implicit~` for `--validation`). Python 3.13 for `uctest` (`py -3.13`).
- Live runs: few (one or two per question), never against the real saves (the harness copies them), unlocked (`--fps-limit 0`).
- Commit only on the owner's go-ahead (per task, as in the raster phases).

## Rulings of this plan (differences from the spec, with reasons)

1. **BLAS built per scene packet, not at the boundary.** The spec retains per-packet buffers until the boundary; the raster plan (Ruling 1 of p4-6) found that per-slot palette/uniform buffers cannot outlive their fences cheaply. Instead each scene packet's command buffer runs a transform compute pass (palette applied to its caster vertices, output into an RT-owned per-frame position buffer) and builds that packet's BLAS (one BLAS per packet, all its instances as geometries); the boundary only builds the TLAS over the frame's BLASes. RT-owned buffers live per frame-in-flight, so nothing of the packet is retained.
2. **Instance rule:** the shadow-caster rule of `record_shadow` (mesh, triangle list, perspective, depth write) **including** alpha cut-outs, which become non-opaque geometry (spec: "alpha cut-outs included this time").
3. **Phase 0 is not throwaway code.** The spike is the real AS builder plus a debug probe (`UC_RT_PROBE`); only the probe shader is temporary and is removed when RT shadows land.
4. **Spike document is `docs/spikes/S8-ray-tracing.md`** (S7 is the tessellation survey).

## Review Focus

1. **A frame with no scene (menu, movie, black) or a scene packet with no casters.** Expect no BLAS, no TLAS build, no RT pass, picture unchanged. Task 2 smoke.
2. **The first scene packet after a level change / RT switched on mid-scene.** Expect no use of a BLAS from an earlier frame or a stale TLAS; the frame with a partial capture still renders. Task 2 smoke (toggle 20 times, no growth, `--validation` clean).
3. **A GPU without ray query (or the probe on a non-RTX GPU).** Expect `ray_tracing=false` with a reason in `--features`, RT options ignored, no device-creation failure. Task 1 test.
4. **Very large frames (fight with the leech wave).** Expect the per-frame position/scratch buffers to grow once and stay, not to allocate per frame, and no stall. Task 2 (growth counter) and Task 3 (fight3 window).
5. **Master switch off with levels set.** Expect every raster effect back exactly as configured and the levels kept in `config.json`. Task 4 test.

---

## Phase 0 — Feasibility

### Task 1: Ray tracing device support and feature report

**Files:** Modify `renderer/vulkan_renderer.h/.cpp` (device creation ~lines 185–228), `renderer/upscaler_probe.h/.cpp`, `game/feature_report.h`, `game/uc_main.cpp:216`, `renderer/streamline.h/.cpp`, `cmake/upscaler_sdks.cmake`, `tests/test_feature_report.cpp`.

**Interfaces:**
- Produces:
  - `bool VulkanRenderer::ray_tracing_supported() const` — true when the device was created with all RT requirements (Global Constraints) and the effects path.
  - `UpscalerSupport` (`upscaler_probe.h`) gains `bool ray_tracing; std::string ray_tracing_reason; bool ray_reconstruction; std::string ray_reconstruction_reason;`.
  - `FeatureReport` gains the same four fields, JSON keys `ray_tracing`, `ray_tracing_reason`, `ray_reconstruction`, `ray_reconstruction_reason`.
  - `bool Streamline::ray_reconstruction_supported(VkPhysicalDevice, std::string& reason) const` (feature `sl::kFeatureDLSS_RR`); Streamline loads `kFeatureDLSS_RR` alongside `kFeatureDLSS`.
  - The RT extensions and features (`accelerationStructure`, `rayQuery`, `bufferDeviceAddress`, `descriptorIndexing` + `runtimeDescriptorArray` + `shaderSampledImageArrayNonUniformIndexing`) are enabled only when all are supported; otherwise none of them is enabled.
- [ ] **Step 1: Write the failing test** in `tests/test_feature_report.cpp`: a report with `ray_tracing=true, ray_tracing_reason="", ray_reconstruction=false, ray_reconstruction_reason="no DLSS-RR"` round-trips through JSON; a JSON without the four keys parses to `false` and empty reasons (older exe).
- [ ] **Step 2: Run** `ctest --test-dir build -R test_feature_report --output-on-failure`. Expected: FAIL (does not compile).
- [ ] **Step 3: Implement** the fields, the probe checks (reasons: "the GPU lacks VK_KHR_ray_query", "the GPU lacks VK_KHR_acceleration_structure", "…descriptor indexing", "the effects path is unavailable"), device creation, the Streamline feature and the two DLLs plus licence in `cmake/upscaler_sdks.cmake` (same list as `sl.dlss.dll`/`nvngx_dlss.dll`).
- [ ] **Step 4: Run** the test; then `build\uc.exe --features` on the owner's machine. Expected: PASS, and `"ray_tracing":true`, `"ray_reconstruction":true` whatever `--upscaler` is (RR support is a device capability; using it needs DLSS, which phase 4 checks).
- [ ] **Step 5: Commit.** "Report and enable ray tracing device support".

### Task 2: Per-frame acceleration structures

**Files:** Create `renderer/rt_scene.h/.cpp`, `renderer/shaders/rt_transform.slang`, `renderer/shaders/rt_probe.slang`; modify `renderer/vulkan_renderer.h/.cpp` (a `record_rt` lambda next to `record_shadow`, called on the same condition `writes_scene`; TLAS + probe at the boundary before the occlusion pass), `renderer/pass_timer.h` (`GpuPass::rt_as`, `GpuPass::rt_probe`), `renderer/CMakeLists.txt`, `tests/renderer_effects_smoke.cpp` (`--rt-as` section).

**Interfaces:**
- Consumes: Task 1 (`ray_tracing_supported()`); per draw `DrawGpu` (`vertices`, `indices`, `vertex_offset`, `index_offset`, `index_count`, `first_index`, `first_vertex`, `palette`, `current`, `indexed`), the slot's palette storage buffer, `GpuMeshVertex` layout (`gx_transforms.h`), the alpha word test of `record_shadow`.
- Produces:
  - `std::optional<bool> rt_instance_opaque(bool mesh, bool has_indices, bool perspective, bool z_write, uint32_t alpha_word, VkPrimitiveTopology topology)` in `renderer/rt_scene.h` (inline, pure) — `nullopt`: not an instance; `true`: opaque; `false`: alpha cut-out (alpha compare bits of `pixel[0]` as in `record_shadow`). `record_shadow` uses it too (a caster is `== true`), so the two rules cannot drift.
  - `class uc::RtScene`:
    - `void initialize(VkDevice, VkPhysicalDevice, const VkPhysicalDeviceMemoryProperties&, const std::filesystem::path& shaders, uint32_t frames_in_flight);`
    - `void begin_frame(uint32_t frame_slot);` — resets the frame's BLAS list; buffers are reused.
    - `void record_packet(VkCommandBuffer, std::span<const RtGeometry> geometries);` — transform pass (`rt_transform.slang`: one thread per vertex, palette row selection exactly as `smoke.slang`'s vertex stage, view-space output float3), then one BLAS build (`PREFER_FAST_BUILD`, one geometry per instance, `OPAQUE` flag per instance) into the frame slot's pool.
    - `bool build_tlas(VkCommandBuffer);` — false when the frame has no BLAS; identity transforms, one TLAS instance per packet BLAS, `instanceCustomIndex` = packet index.
    - `VkAccelerationStructureKHR tlas() const;` `uint64_t buffer_growths() const;` (test hook, counts reallocations) `struct Stats { uint32_t blas, geometries; uint64_t triangles; }; Stats stats() const;`.
  - `RtGeometry { VkDeviceAddress vertices, indices, positions_out; uint32_t vertex_count, index_count, first_index; int32_t first_vertex; uint32_t palette, current, indexed; bool opaque; }` (builder fills from `DrawGpu`).
  - Frame log fields `gpu_rt_as_ms`, `rt_blas`, `rt_triangles`; `--profile` lists `rt_as`.
  - Debug probe: with `UC_RT_PROBE=1` and RT supported, the boundary runs `rt_probe.slang` (one shadow ray per internal pixel from the reconstructed view position toward the dominant directional light, opaque geometry only for the probe, result written nowhere but a 1×1 counter of occluded pixels) timed as `GpuPass::rt_probe` → frame log `gpu_rt_probe_ms`. Without the variable nothing RT runs in this phase.
- [ ] **Step 1a: Write the failing unit test** `tests/test_rt_instances.cpp` (register in `tests/CMakeLists.txt`): an opaque perspective depth-writing indexed mesh triangle list → `true`; the same with an alpha compare other than "always" → `false`; non-mesh, no indices, no depth write, not perspective, or a strip topology → `nullopt`.
- [ ] **Step 1b: Write the failing smoke section** `--rt-as` on the contact-shadow fixture (box on a floor, directional light): with `UC_RT_PROBE=1` the probe counts > 0 occluded pixels and the count equals the number of floor pixels inside the box's shadow footprint computed on the CPU from the fixture geometry (±2%); moving the box out of the light's path gives 0; a frame without a scene builds nothing (`stats().blas==0`, `pass_runs(GpuPass::rt_as)` unchanged); toggling the probe 20 times leaves `buffer_growths()` constant after the first frame; `--validation` clean (Review Focus 1, 2).
- [ ] **Step 2: Run** `ctest --test-dir build -R test_rt_instances --output-on-failure` and `renderer_effects_smoke.exe --rt-as --validation`. Expected: FAIL (missing).
- [ ] **Step 3: Implement** `RtScene`, the shaders, the `record_rt` hook and the boundary TLAS + probe. Buffers (positions, scratch, BLAS storage, TLAS, instances) grow geometrically on demand and never shrink (Review Focus 4). Synchronisation: a buffer barrier transform → BLAS build in the packet, `ACCELERATION_STRUCTURE_BUILD` → `ACCELERATION_STRUCTURE_BUILD` before the TLAS, TLAS → compute read before the probe.
- [ ] **Step 4: Run** the section and `ctest`. Expected: PASS, validation clean.
- [ ] **Step 5: Commit.** "Build per-frame ray tracing acceleration structures".

### Task 3: Measure on a real fight and decide (S8)

**Files:** Create `docs/spikes/S8-ray-tracing.md`.

- [ ] **Step 1: RT-off baseline on the owner's settings.** `py -3.13 -m uctest run fight --game-arg=--upscaler --game-arg=dlss --game-arg=--profile` at speed 1 with the output at 2560×1440, DLSS Quality: record the median GPU frame time (frame log) and FPS. One run.
- [ ] **Step 2: Probe run.** Same command with `UC_RT_PROBE=1`: record `gpu_rt_as_ms` and `gpu_rt_probe_ms` (median and p95), `rt_blas`, `rt_triangles`, plus one replay of the recorded `fight3` demo (its heavy leech-wave window is the worst case for AS size) with `UC_RT_PROBE=1`. One run each.
- [ ] **Step 3: Off-path check.** `py -3.13 -m uctest bench dist/uc_before.exe dist/uc.exe --demo fight2 --runs 3` (before = `main` without Tasks 1–2). Expected: within 1%.
- [ ] **Step 4: RR check.** Confirm in S8: `sl.dlss_d` reports supported on the 4080 (Task 1 output), the licence file of `nvngx_dlssd.dll` permits redistribution next to `nvngx_dlss.dll`, and the DLSS path already passes jitter and the projection matrices (`dlss_upscaler.cpp`) that `slDLSSDSetOptions`/constants need. If any fails: RR drops out and the own denoiser runs everywhere (spec).
- [ ] **Step 5: Decide.** Write S8: measurements, the remaining High budget = `1000/120 − RT-off frame ms`, a re-split of the spec's High/Ultra budget table from the measured AS and per-ray cost (one probe ray per pixel ≈ the cost of one full-res ray of any effect). Go when the remaining High budget ≥ 2 ms; otherwise S8 lists the options (lower High presets, or a lower FPS target) and **work stops for the owner's decision** before Task 4.
- [ ] **Step 6: Commit** S8 (on the owner's go-ahead).

## Phase 1 — Infrastructure

### Task 4: RT options model, master switch and raster replacement

**Files:** Modify `renderer/graphics_effects.h`, `game/options.h/.cpp` (`parse_options`, usage text), `game/options_config.cpp`, `game/live_settings.cpp`, `renderer/vulkan_renderer.cpp` (the passes read the effective block); tests `tests/test_options.cpp`, `tests/test_options_config.cpp`, `tests/test_live_settings.cpp`, new `tests/test_rt_effects.cpp` (register in `tests/CMakeLists.txt`).

**Interfaces:**
- Produces (in `graphics_effects.h`, namespace as the file):
  - `GraphicsEffects` gains `bool ray_tracing=false; EffectLevel rt_reflections=EffectLevel::off, rt_lighting=EffectLevel::off, rt_shadows=EffectLevel::off, rt_local_shadows=EffectLevel::off, rt_ambient_occlusion=EffectLevel::off;`.
  - `EffectLevel rt_level(const GraphicsEffects&, EffectLevel level, bool rt_supported)` — `level` when `ray_tracing && rt_supported`, else `off`.
  - `bool any_rt_active(const GraphicsEffects&, bool rt_supported)`.
  - `GraphicsEffects effective_effects(const GraphicsEffects& chosen, bool rt_supported)` — a copy where active RT shadows set `shadows=off, contact_shadows=false` and active RT AO sets `ambient_occlusion=off`; RT levels of the copy are already masked by `rt_level`. The renderer stores the chosen block (config) and runs every pass from the effective one.
  - `needs_scene_targets` is true when `any_rt_active` (it takes `rt_supported`, or the renderer passes the effective block; builder's call, one call site style).
- [ ] **Step 1: Write the failing tests** `tests/test_rt_effects.cpp`:
  - master off with `rt_shadows=high, shadows=medium, contact_shadows=true, ambient_occlusion=low, rt_ambient_occlusion=ultra`: `effective_effects` equals the input with every RT level `off` (raster untouched) (Review Focus 5).
  - master on, same input: `shadows==off && !contact_shadows && ambient_occlusion==off`, `rt_shadows==high`, `rt_ambient_occlusion==ultra`.
  - master on, `rt_supported=false`: identical to master off.
  - master on, only `rt_reflections=low`: raster shadows/AO untouched.
  - `needs_scene_targets` true for master on + any RT level, false for master on with all RT levels off and no raster effect.
  - `test_options_config`: the six keys round-trip; a config without them loads defaults; an invalid level string is rejected like the existing levels.
  - `test_options`: `--ray-tracing --rt-shadows high` parses; `--rt-shadows bogus` is an error.
  - `test_live_settings`: changing only `ray_tracing` reaches `set_effects` as a live change (no restart).
- [ ] **Step 2: Run** `ctest --test-dir build -R "test_rt_effects|test_options|test_live_settings" --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement** the fields, functions, config/CLI/usage, and in `VulkanRenderer::set_effects` keep `chosen_effects_` and derive `effects_=effective_effects(chosen_effects_, ray_tracing_supported())`. The `UC_RT_PROBE` gate of Task 2 becomes "probe only while no RT effect exists": leave it as is.
- [ ] **Step 4: Run** the tests and the full `ctest`. Expected: PASS.
- [ ] **Step 5: Commit.** "Add ray tracing options with a master switch that replaces raster counterparts".

### Task 5: Albedo and material-record target

**Files:** Create `renderer/rt_material.h` (pure), `tests/test_rt_material.cpp`; modify `renderer/shaders/smoke.slang` (a fourth attachment write in the motion variants, behind a push-constant bit), `renderer/graphics_effects.h` (`kEffectRtMaterial` bit), `renderer/vulkan_renderer.h/.cpp` (attachment creation in `rebuild()` while `any_rt_active`, a per-frame record buffer filled per scene packet), `renderer/gbuffer.h` (`kGBufferModelValid` from the reserved bits), `tests/renderer_effects_smoke.cpp` (`--rt-material` section).

**Interfaces:**
- Consumes: `gx::TevMaterial` (`gx_materials.h`), `gx::ChannelLighting`/`channel_lighting` (`scene_lights.h`), the draw's GX lights block.
- Produces:
  - `struct RtMaterialRecord { std::array<float,4> material_color, ambient_color; uint32_t light_mask; uint32_t lights_offset; uint32_t flags; float roughness; };` (std430, 48 bytes; flags: `kRtModelValid=1, kRtReflective=2, kRtEmissive=4`). `lights_offset` indexes a per-frame array of the draw's 8 GX lights (view space, as `DrawState.lights`).
  - `bool light_model_valid(const gx::TevMaterial&, const gx::ChannelLighting& channel0)` — true when channel 0 is lit and the TEV's colour result is the rasterised lit colour multiplied by a texture or constant in stage 0 (with any later stages that only pass the previous result through, fog aside); false otherwise (spec: unmodelled pixels are left untouched).
  - Attachment `scene_material_`: `R32_UINT` record index per pixel (`~0u` = no record) and `scene_albedo_` `R8G8B8A8_UNORM` (stage-0 texture × material colour, unlit), written by depth-writing scene draws only while `kEffectRtMaterial` is set; absent and unbound otherwise (no cost with RT off).
  - `VkBuffer VulkanRenderer` per-frame `rt_records_` / `rt_lights_` storage buffers, grow-only.
- [ ] **Step 1: Write the failing tests** `tests/test_rt_material.cpp`: `light_model_valid` true for "stage 0 = texture × rasterised colour, one stage"; true with a second pass-through stage; false for an unlit channel 0; false for an additive TEV (`texture + rasterised`); false for a stage that uses the rasterised colour only as alpha; `sizeof(RtMaterialRecord)==48`. Smoke `--rt-material`: with RT shadows forced on in the test effects, the fixture box's pixels carry a record whose `material_color` equals the fixture's material colour and whose flags include `kRtModelValid`; the albedo target holds the fixture texture colour (±1); with RT off the attachments do not exist (`scene_material_` null) and the frame is identical to before Task 5.
- [ ] **Step 2: Run** `ctest --test-dir build -R test_rt_material --output-on-failure` and `renderer_effects_smoke.exe --rt-material --validation`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** both, plus `renderer_effects_smoke.exe --validation` (every section) and `ctest`. Expected: PASS.
- [ ] **Step 5: Off-path bench:** `py -3.13 -m uctest bench dist/uc_before.exe dist/uc.exe --demo fight2 --runs 3`. Expected: within 1%.
- [ ] **Step 6: Commit.** "Write albedo and material records for ray-traced effects".

### Task 6: Own denoiser

**Files:** Create `renderer/rt_denoiser.h/.cpp`, `renderer/shaders/rt_denoise.slang`, `tests/test_rt_denoise_math.cpp` (pure helpers); modify `renderer/pass_timer.h` (`GpuPass::rt_denoise`), `renderer/CMakeLists.txt`, `tests/renderer_effects_smoke.cpp` (`--rt-denoise` section).

**Interfaces:**
- Consumes: `scene_depth_`, `scene_motion_`, the G-buffer normal (`gbuffer.h` decode), `tracker_.camera_cut()`.
- Produces:
  - `enum class RtSignal : uint32_t { visibility, ambient_occlusion, radiance };` — visibility/AO: one channel, no variance guide; radiance: RGB with variance-guided luminance weights (spec).
  - `class uc::RtDenoiser { void initialize(VkDevice, const VkPhysicalDeviceMemoryProperties&, const std::filesystem::path& shaders); void resize(VkExtent2D signal_extent); VkImageView denoise(VkCommandBuffer, RtSignal, VkImageView noisy, const DenoiseInputs&, uint32_t atrous_iterations); void reset(); }` — one instance per signal, owning its history images; `reset()` on camera cuts and resizes.
  - Temporal: reprojection by the motion vectors, rejection when reprojected depth differs by more than 2% of view depth or normals by more than 25°, history length capped at 32 frames (exponential blend `max(1/len, 1/32)`); spatial: 5×5 à-trous, `atrous_iterations` passes, edge-stopping on depth, normal and (radiance only) luminance.
  - Pure helpers in `rt_denoise.h`-adjacent header: `float history_alpha(uint32_t length)`, `bool reproject_valid(float depth, float history_depth, float normal_dot)` — with the thresholds above.
- [ ] **Step 1: Write the failing tests:** `test_rt_denoise_math` pins `history_alpha(1)==1`, `history_alpha(32)==history_alpha(100)==1/32.f`, `reproject_valid` true at 1.9% depth difference and dot 0.91, false at 2.1% or dot 0.90. Smoke `--rt-denoise`: a fixture of a constant 0.5 visibility image with per-pixel white noise (amplitude 0.5, fixed seed) on a still camera: after 16 frames the standard deviation of the output is < 0.05 and the mean within 0.02 of 0.5; a hard edge in the noise-free signal stays within 2 px of its position (no blur across a depth edge of the fixture); a camera cut resets history (frame after the cut has the single-frame spatial result, not the old mean); `--validation` clean.
- [ ] **Step 2: Run** the test and the section. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** both and `ctest`. Expected: PASS.
- [ ] **Step 5: Commit.** "Add a temporal and à-trous denoiser for ray-traced signals".

## After this plan

- Phase 2–6 plan (`docs/superpowers/plans/…-ray-tracing-p2-6.md`) written from S8: RT shadows and local shadows (they bring the UI group with the master checkbox, the greyed "заменено трассировкой лучей" raster options and the 13-language strings), RT AO, Ray Reconstruction, reflections with `dist/rt_materials.json` and `UC_RT_DEBUG=reflective`, lighting. The probe shader of Task 2 is removed when RT shadows land.
- `docs/graphics-effects.md` gains a ray tracing section with the first effect.
