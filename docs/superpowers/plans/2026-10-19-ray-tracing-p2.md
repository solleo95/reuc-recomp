# Ray tracing, phase 2 (RT shadows and RT local shadows) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (the owner chose native execution in this session, 2026-10-19). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the first two ray-traced effects work and visible: RT shadows (directional lights) and RT local shadows (point and spot lights), with the master switch, the launcher and in-game settings, the greyed raster counterparts, presets Off/Low/Medium/High/Ultra and the budget measured on a real fight.

**Architecture:** One compute pass (`RtShadowPass`, `shaders/rt_shadow.slang`) runs at the scene boundary after the TLAS build (phase 1). For each scene pixel it takes the pixel's material record (`scene_material_`, phase 1) and that draw's 8 GX lights, evaluates each light's contribution with the GX lighting equations, shoots one inline ray query per selected light and writes ONE visibility factor `1 − Σ share_i·(1 − visibility_i)` (the share of the pixel's light each shadowed light removes). The own denoiser (`RtDenoiser`, phase 1) filters that signal; a composite multiplies the scene colour by the factor on pixels the light model can describe, faded by GX fog. Directional lights follow the `rt_shadows` level, point and spot lights the `rt_local_shadows` level.

**Tech Stack:** C++20, Vulkan 1.3 + `VK_KHR_ray_query`, Slang, the phase 0–1 components (`RtScene`, `RtDenoiser`, `RtMaterialRecord`, `effective_effects`).

**Spec:** `docs/superpowers/specs/2026-10-19-ray-tracing-design.md` (read "Options", "RT replaces its raster counterpart", "How it renders" items 4–5, "Presets"). Measurements: `docs/spikes/S8-ray-tracing.md`. Phases 0–1: `docs/superpowers/plans/2026-10-19-ray-tracing-p0-1.md`.

## Global Constraints

- No guest-side work, no packet change; RT applies only to draws before the scene boundary in a frame with a scene.
- Targets (owner): all RT options on High ≈ 100 FPS (≤ 2.9 ms all RT work), all on Ultra ≥ 60 FPS (≤ 9.5 ms), Low ≤ 0.6 ms, Medium ≤ 1.2 ms (RTX 4080, 1440p DLSS Quality). FPS targets win over the ms budgets; a preset that misses its budget gives up rays or resolution first. Phase 0 unit costs: AS build + capture 0.45 ms GPU (+≈0.6 ms CPU side), one full-resolution (1707×1280) shadow-ray pass ≈ 1.0 ms.
- RT off (master off, or both shadow levels off) costs nothing extra: no pass records, nothing allocates; the picture equals today's.
- Replacement: while RT shadows are active the shadow map and contact shadows do not run (`effective_effects`, phase 1); the UI greys them with the reason "заменено трассировкой лучей".
- "No option is shown before its effect works": this phase shows the master checkbox, RT shadows and RT local shadows only (the other three RT options stay out of the UI until their phases).
- Scene colour stays RGBA8; shadows scale lit colour only on pixels the light model can describe (`kGBufferLit` + `kGBufferModelValid`), faded by the G-buffer fog factor.
- Languages: 13 in `launcher/translations.inc`.
- Source edits with Edit/Write only (no sed/python/heredoc patches). Build `build.bat --no-install` through PowerShell (never a bare `cmake --build` outside `scripts/env.bat`); tests `ctest --test-dir build --output-on-failure`; GPU smoke from `build/renderer` (`VK_LOADER_LAYERS_DISABLE=~implicit~` for `--validation`).
- Live/bench runs: harness only, windowed 1280×720 (never fullscreen or desktop-sized; `--game-arg=--render-scale 2` for the 1440p-equivalent GPU load), few runs, never the real saves; confirm every demo run is combat (≈149k draws per 60 retraces for `fight2`; a flat ≈66k is a menu).
- The owner has unrelated uncommitted edits in `AGENTS.md`, `scripts/uctest/*.py`, `tests/test_uctest_autoplay.py`: never stage them; `git add` explicit paths. Commit only when a task's checks are green; no Co-Authored-By/AI attribution.

## Rulings of this plan

1. **One pass for both shadow options**, driven by the pixel's own record lights (not the frame-wide `CollectedLight` list): each pixel knows exactly which lights its draw used (`light_mask`, `lights_offset`), so the "share of the pixel's light" is computed from real GX data. This supersedes the spec's "estimate from collected lights"; the result is the same factor, with exact attenuation coefficients.
2. **Alpha cut-outs cast no RT shadow in this phase** (non-opaque geometry is culled in the shadow ray, as the probe and the raster shadow map do); sampling the cut-out texture alpha in the candidate loop is listed as later work.
3. **Phase order inside the phase:** math and presets, pass, wiring, UI, tuning, probe removal. The probe (`UC_RT_PROBE`) stays until Task 12 so the phase 0 numbers can be compared.
4. **Initial preset table is a starting point:** rays and light counts follow the spec; the resolution divisor (1, 2, 4 of the internal resolution) is the tuning knob of Task 11, because one full-resolution ray pass costs ≈ 1.0 ms and High must fit in ≈ 0.7 ms for both shadow options together.

## Review Focus

1. **Scene without a directional light, or a pixel whose draw has no lights** (menu, movie, black frame, unlit particles). Expect factor 1, picture unchanged. Task 8 and 9 smoke.
2. **Light switching on/off between frames, camera cut.** Expect the denoiser history to reset on a cut and no stale factor applied after the scene changes. Task 9.
3. **Master switch toggled live with levels kept; level changes mid-scene.** Expect no per-frame allocation after warm-up, no validation errors, raster shadows back exactly as configured. Task 9, 10.
4. **Pixels the light model cannot describe (baked colour, emissive, unmodelled TEV, cut-outs).** Expect untouched colour, never a double darkening. Task 9.
5. **Soft shadows on a still camera.** Expect no visible shimmer after the denoiser's history fills (noise sd below the Task 8 pin). Task 8/9.

---

## Task 7: Shadow math and presets (pure host code)

**Files:** Create `renderer/rt_shadow_math.h`, `tests/test_rt_shadow_math.cpp` (register in `tests/CMakeLists.txt`).

**Interfaces:**
- Consumes: `gx::LightUniform` (`gx_materials.h`: `color, cosine, distance, position, direction`), `EffectLevel`.
- Produces (namespace `uc`):
  - `struct RtShadowPreset { uint32_t divisor; uint32_t rays; bool soft; };` and `RtShadowPreset rt_shadow_preset(EffectLevel)` for directional lights: low {4,1,false}, medium {2,1,false}, high {2,1,true}, ultra {1,2,true}; `off` → `{0,0,false}`.
  - `struct RtLocalPreset { uint32_t divisor; uint32_t lights; bool soft; };` and `RtLocalPreset rt_local_preset(EffectLevel)`: low {4,1,false}, medium {2,2,false}, high {2,4,true}, ultra {1,8,true}; `off` → `{0,0,false}`.
  - `float gx_light_contribution(const gx::LightUniform&, bool directional, std::array<float,3> position, std::array<float,3> normal)` — luminance (0.299,0.587,0.114 of `color`) × max(N·L,0) × the GX attenuation (distance coefficients for point and spot lights, the spot's angular term from `cosine`/`direction`; directional lights unattenuated). Must equal the shader's evaluation (Task 8 tests a fixture against it); copy the equations from `lightControl` in `renderer/shaders/smoke.slang` rather than re-deriving.
  - `float shadow_factor(float ambient_luminance, std::span<const float> contributions, std::span<const float> visibilities)` = `1 − Σ share_i·(1 − v_i)` with `share_i = c_i / (ambient + Σ c)`; 1 when the denominator is 0.
  - `constexpr float kRtSunConeRadians=0.02f; constexpr float kRtLightRadiusFraction=0.03f;` (soft-shadow sizes, this plan's constants; tuned in Task 11).
- [ ] **Step 1: Write the failing test** `tests/test_rt_shadow_math.cpp`: `rt_shadow_preset(low)=={4,1,false}`, `ultra=={1,2,true}`, `off.divisor==0`; `rt_local_preset(ultra).lights==8`, `medium.lights==2`; `shadow_factor(0.2,{0.8},{0})==0.2f` (±1e-6), `shadow_factor(0.2,{0.8},{1})==1`, `shadow_factor(0.2,{0.4,0.4},{0,1})==0.6f`, `shadow_factor(0,{},{})==1`; a directional light with colour (1,1,1), N·L=0.5 contributes 0.5; a point light with distance coefficients `{1,0,1,0}` at d=2 contributes N·L×1/5.
- [ ] **Step 2: Run** `ctest --test-dir build -R test_rt_shadow_math --output-on-failure`. Expected: FAIL (does not compile).
- [ ] **Step 3: Implement** `rt_shadow_math.h` (header only).
- [ ] **Step 4: Run** the test. Expected: PASS.
- [ ] **Step 5: Commit.** "Add ray-traced shadow presets and light-share math".

## Task 8: The RT shadow pass

**Files:** Create `renderer/rt_shadow_pass.h/.cpp`, `renderer/shaders/rt_shadow.slang` (entry points `traceMain`); modify `renderer/CMakeLists.txt` (shader and sources like `rt_denoise.slang`), `renderer/pass_timer.h` (`GpuPass::rt_shadows`), `renderer/rt_scene.h` (accessors the pass needs: the TLAS, already `tlas()`), `tests/renderer_effects_smoke.cpp` (`--rt-shadows`, fixture helpers shared with `--rt-as`).

**Interfaces:**
- Consumes: Task 7 presets and constants; `RtScene::tlas()`; `scene_depth_` (D32), the G-buffer (`gbuffer.slang`: normal, flags, fog), `scene_material_` (R32_UINT record index per pixel), the frame's `rt_records_` and `rt_lights_` buffers (`RtMaterialRecord` 48 B, `LightUniform` 8 per record at `lights_offset`).
- Produces:
  - `struct RtShadowInputs { VkCommandBuffer command; VkAccelerationStructureKHR tlas; VkImageView depth, gbuffer, material; VkBuffer records, lights; VkExtent2D region; std::array<float,6> viewport; ViewParams view; EffectLevel sun, local; uint32_t frame; uint64_t targets_generation; uint32_t slot; };`
  - `class RtShadowPass { void initialize(VkDevice, const VkPhysicalDeviceMemoryProperties&, const std::filesystem::path& shaders, std::function<void()> idle={}); VkImageView record(const RtShadowInputs&); VkExtent2D signal_extent() const; VkImage signal_image() const; void shutdown(); }` — `record` dispatches the trace pass at `region / divisor` (divisor = the smaller of the two active presets' divisors; 1 = full) and returns the noisy factor image (R8, `VK_IMAGE_LAYOUT_GENERAL`, written with its own barrier). Nothing is allocated until `record` is called.
  - Shader behaviour per signal pixel (centre of its `divisor`×`divisor` block): read the pixel's depth, G-buffer flags (skip with factor 1 unless `kGBufferLit` and `kGBufferModelValid`, or a cut-out) and record index (factor 1 when `kRtNoRecord`); reconstruct the view position (`view_space.slang reconstructView`, as `rt_probe.slang`); for each of the record's lights in `light_mask` evaluate the contribution exactly as `gx_light_contribution`; classify directional vs point/spot (`classify_light` rule: GX directional lights have no distance term — use the same test as `scene_lights.cpp`); pick the directional lights when `sun!=off` (`rays` rays each, rotated by blue noise over a cone of `kRtSunConeRadians` when `soft`) and the strongest `lights` point/spot lights when `local!=off` (one ray each toward a point on the light's disc of radius `kRtLightRadiusFraction × distance` when `soft`); a ray is `TMin = 1e-3·|p| + 1e-3` (as the probe), opaque geometry only (`RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH`), `TMax` = the distance to the light (1e4 for directional); output `shadow_factor(ambient_luminance, contributions, visibilities)` with the record's `ambient_color` luminance.
- [ ] **Step 1: Write the failing smoke section** `--rt-shadows` in `renderer_effects_smoke.cpp` on the existing box-on-floor fixture with RT forced on through the effective block (`ray_tracing=true`, `rt_shadows=medium`; same way `--rt-material` forces it): (a) directional light: the signal at floor pixels inside the box's shadow footprint is < 0.35, on the lit floor 1.0 (±0.01), and equals `shadow_factor` computed on the CPU from the fixture's light and the record's ambient (±0.05) at three sample pixels; (b) a point-light fixture (same scene, point light at a known position, `rt_local_shadows=medium`): the shadow of the box falls away from the light and the factor inside it is < 0.5; (c) a scene whose draws have no lights (unlit material) → all factors 1.0; (d) `rt_shadows=off, rt_local_shadows=off` → `record` is not called by the frame and nothing is allocated (`buffer_growths` and the pass's image count unchanged); (e) toggling the level 20 times creates no new images after the first frame at each size; `--validation` clean.
- [ ] **Step 2: Run** `renderer_effects_smoke.exe --rt-shadows --validation`. Expected: FAIL (section missing / pass not built).
- [ ] **Step 3: Implement** the pass, shader and the CMake wiring; add `GpuPass::rt_shadows` (check how `pass_timer.h` consumers size their arrays: `game/uc_main.cpp` names passes explicitly, add the name there).
- [ ] **Step 4: Run** the section, then every smoke section with `--validation`, then `ctest`. Expected: PASS, 0 validation errors.
- [ ] **Step 5: Commit.** "Add the ray-traced shadow pass".

## Task 9: Wire shadows into the frame (denoise, composite, replacement)

**Files:** Modify `renderer/vulkan_renderer.h/.cpp` (`record_upscale` boundary block next to the probe; capture gating), `renderer/shaders/rt_shadow.slang` (`compositeMain`), `renderer/rt_shadow_pass.h/.cpp` (composite recording), `tests/renderer_effects_smoke.cpp` (`--rt-shadows-frame`).

**Interfaces:**
- Consumes: Task 8; `RtDenoiser` (`denoise(command, RtSignal::visibility, noisy, DenoiseInputs, atrous_iterations)`, `reset()`, `resize()`); `effective_effects`; the phase 1 `rt_frame_`/`rt_as_open_` capture machinery.
- Produces:
  - `bool VulkanRenderer::rt_scene_active() const` = `rt_probe_active() || rt_shadows_active()`, with `rt_shadows_active()` = `ray_tracing_ && (effects_.rt_shadows!=off || effects_.rt_local_shadows!=off)`; every current use of `rt_probe_active()` that decides AS capture, TLAS build, `rt_` initialisation, scene targets, lights, G-buffer and the RT material target becomes `rt_scene_active()`; the probe itself stays gated by `rt_probe_active()`.
  - Boundary order in `record_upscale`: TLAS (existing) → `RtShadowPass::record` → denoise (`atrous_iterations` = 2 low/medium, 3 high/ultra; `reset()` on `tracker_.camera_cut()`, black frames and target rebuilds; `resize()` when the signal extent changes) → `RtShadowPass::composite(command, filtered_view, color, ...)` multiplying `color.rgb` by `lerp(1, factor, 1 − fog)` on the same pixel test as the trace (`kGBufferLit && kGBufferModelValid`, no cut-outs), using a depth-aware bilinear upsample from the signal resolution (3×3 taps weighted by depth difference, as `upsampleOcclusion` in `gtao.slang`) → the existing occlusion pass (its raster shadow/contact inputs are already off through `effective_effects`) → upscaler.
  - Timers: `GpuPass::rt_shadows` around trace + denoise + composite; frame log `gpu_rt_shadows_ms`, `--profile` lists it.
- [ ] **Step 1: Write the failing smoke section** `--rt-shadows-frame`: render the box/floor fixture through the real boundary with RT shadows `medium` and the raster `shadows=medium` also set in the chosen block: (a) the presented floor right of the box is ≥ 25 levels darker than with RT off, the far floor within 1 level (Review Focus 5 pin: on a still camera after 16 frames the standard deviation of the shadowed floor patch is < 2 levels); (b) the raster shadow map did not run (`pass_runs(GpuPass::shadow)` unchanged, i.e. replacement works) and the raster setting is still `medium` in the chosen block; (c) a frame without a scene and a scene with an unlit box change nothing (Review Focus 1, 4); (d) master off with the levels kept → identical to RT off, raster shadows run again; (e) a camera cut resets the history (the frame after a cut is not the long-term average of the previous scene); (f) 20 level/master toggles allocate nothing after the first frame at each size; `--validation` clean.
- [ ] **Step 2: Run** `renderer_effects_smoke.exe --rt-shadows-frame --validation`. Expected: FAIL.
- [ ] **Step 3: Implement** the gating change, the boundary sequence, the composite entry point and the timer; `needs_scene_targets`/`effects_need_lights`/`effects_need_gbuffer` already include `any_rt_active` (phase 1).
- [ ] **Step 4: Run** the section, every smoke section with `--validation`, `ctest`. Expected: PASS.
- [ ] **Step 5: Commit.** "Apply ray-traced shadows to the scene colour".

## Task 10: Settings in the launcher and the in-game menu

**Files:** Modify `launcher/main.cpp` (Graphics page, effects group ~lines 480–530), `game/settings_menu.cpp` (Effects tab ~lines 205–230), `launcher/translations.inc`, `game/options.cpp` / `game/uc_main.cpp` (usage text: list `--ray-tracing`, `--rt-shadows`, `--rt-local-shadows` again; the other three RT flags stay hidden), `tests/test_settings_menu.cpp`, `docs/graphics-effects.md`.

**Interfaces:**
- Consumes: `FeatureReport.ray_tracing/ray_tracing_reason` (phase 1), the existing `level_combo` / `toggle` / `compact_checkbox` helpers in both UIs.
- Produces: translation keys (all 13 languages): `ray_tracing` "Трассировка лучей" / "Ray tracing", `rt_shadows` "Тени с трассировкой лучей" / "Ray-traced shadows", `rt_local_shadows` "Локальные тени с трассировкой лучей" / "Ray-traced local shadows", `replaced_by_rt` "Заменено трассировкой лучей" / "Replaced by ray tracing", `rt_unavailable` (the tooltip prefix before the report's reason).
  - Both UIs: a "Ray tracing" group under the effects block: the master checkbox, then two level combos (disabled while the master is unchecked, levels kept visible); the whole group disabled with `features.ray_tracing_reason` as tooltip when `!features.ray_tracing`; `contact_shadows` and (when present in a UI) `shadows` are disabled with `replaced_by_rt` while their RT replacement is active (`rt_shadows` level != off and master on), their saved values untouched.
- [ ] **Step 1: Write the failing tests** in `tests/test_settings_menu.cpp` (mirror the motion-blur/contact-shadow cases at lines 106–117 and 177): setting the master and `rt_shadows=high` through the menu commits `effects.ray_tracing && effects.rt_shadows==high` to `live.options()` and saves them; with the report `ray_tracing=false` the group is disabled (query the menu's `rt_available()` accessor, add it) and a saved `ray_tracing=true` stays visible and can be switched off; the replaced state: `menu.contact_replaced()` is true exactly when master on and `rt_shadows!=off`.
- [ ] **Step 2: Run** `ctest --test-dir build -R "test_settings_menu|test_options" --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement** both UIs, the translations, the usage text and a "Ray tracing" section in `docs/graphics-effects.md` (table rows for the three options, replacement rules, budget, `uc.exe --features` fields).
- [ ] **Step 4: Run** the tests and `ctest`; launch `build\game\uc_launcher.exe` through the harness-free path only to check it starts (no interaction): `build.bat --no-install` must pass. Expected: PASS.
- [ ] **Step 5: Commit.** "Add ray tracing settings to the launcher and the in-game menu".

## Task 11: Measure, tune the presets, check live

**Files:** Modify `renderer/rt_shadow_math.h` (preset divisors/rays/lights), `docs/spikes/S8-ray-tracing.md` (new "Phase 2" section) or a new `docs/spikes/S9-rt-shadows.md`; `tests/test_rt_shadow_math.cpp` for changed presets.

- [ ] **Step 1: GPU cost per preset.** Run the `fight3` demo replay (harness, windowed 1280×720, `--game-arg=--upscaler --game-arg=dlss --game-arg=--upscale-mode --game-arg=quality --game-arg=--render-scale --game-arg=2 --game-arg=--profile`, `UC_FRAME_LOG`) once per setting: RT off; shadows+local `low`, `medium`, `high`, `ultra` (CLI `--ray-tracing --rt-shadows X --rt-local-shadows X`). Record `gpu_rt_as_ms`, `gpu_rt_shadows_ms`, and the end-to-end `interval_ms` median/p95. Check each run is combat (draws per 60 retraces, as in S8).
- [ ] **Step 2: Fit the budgets.** Required: the RT shadow work of High (AS + both shadow options, GPU) ≤ 0.7 ms and its end-to-end cost ≤ 1.0 ms; Low ≤ 0.6 ms and Medium ≤ 1.2 ms end to end for everything RT; Ultra ≤ 9.5 ms for the full set later (this phase uses ≤ 3 ms of it). When a preset misses its budget lower its resolution (`divisor` up to 4) or rays before anything else, keep the preset names and the strict ordering low < medium < high < ultra in cost, update `rt_shadow_math.h` and its tests, remeasure the changed settings once.
- [ ] **Step 3: Off-path A/B.** `py -3.13 -m uctest bench <baseline exe> dist/uc.exe --demo fight2 --runs 3`, baseline = the same commit with `ray_tracing=false` forced through config (`--ray-tracing` absent) — i.e. compare RT-capable-but-off against the phase 1 head to see the phase 2 code adds nothing when off; report the CI honestly (it cannot resolve below ≈ 2%).
- [ ] **Step 4: Live look.** One harness run (`uctest run fight --measure 20` with `--ray-tracing --rt-shadows high --rt-local-shadows high`) with captured frames at the slowdown point with RT shadows off and on; read the PNGs and describe what changed; confirm no shimmer in a short capture sequence (3 frames 100 ms apart).
- [ ] **Step 5: Document and commit** the measurements and the final preset table; "Tune ray-traced shadow presets to the budget".

## Task 12: Remove the phase 0 probe

**Files:** Delete `renderer/shaders/rt_probe.slang`; modify `renderer/rt_scene.h/.cpp` (`record_probe`, `probe_occluded`, counters, probe pipeline), `renderer/vulkan_renderer.h/.cpp` (`rt_probe_`, `rt_probe_active`, `set_rt_probe`, `rt_probe_occluded`, `GpuPass::rt_probe` and its name in `game/uc_main.cpp`), `renderer/CMakeLists.txt`, `tests/renderer_effects_smoke.cpp` (`--rt-as` and its helpers: the AS checks (BLAS/triangle counts, no-scene/no-instance frames, growth) stay and read `RtScene::stats()`; the occluded-pixel comparison moves to `--rt-shadows`, already covered), `docs/spikes/S8-ray-tracing.md` (note that the probe was removed).
- [ ] **Step 1: Remove** the probe code paths and rewrite `--rt-as` so it no longer needs `UC_RT_PROBE`: it enables RT shadows through the effective block instead (the TLAS is then built by the real path).
- [ ] **Step 2: Run** `renderer_effects_smoke.exe --validation` (every section), `ctest`, `build.bat --no-install`. Expected: PASS; `grep -ri "UC_RT_PROBE\|rt_probe" renderer game tests` finds nothing but the S8 note.
- [ ] **Step 3: Commit.** "Remove the ray tracing debug probe".

## Finish

Final read of the whole phase diff by a fresh reviewer (one subagent for the review only, on the most capable model; implementation stays native), then `build.bat` (full install to the game folder per the owner's deploy habit: move test leftovers `dist/config.json`, `dist/debug`, `dist/cpu-*`, stray `uc_*.exe/pdb` out of `dist` first) and hand over for the owner's check; merging stays the owner's call.

## Later phases (unchanged)

RT ambient occlusion (replaces GTAO) → DLSS Ray Reconstruction → RT reflections (+ `dist/rt_materials.json`, `UC_RT_DEBUG=reflective`) → RT lighting. Carry-forwards from phase 1 that must be settled before the denoiser feeds RR or moving cameras are judged: depth rejection without camera-motion compensation (`rt_denoise.slang`), the caller barrier contract for `filtered_` reuse, cut-out shadows with alpha sampling, the unproven RT-off cost of the 5-attachment pipelines.
