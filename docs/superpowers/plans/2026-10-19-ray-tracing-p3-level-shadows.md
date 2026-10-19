# Ray tracing, phase 3 (shadows of the level) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (native, as in phase 2). Steps use checkbox (`- [ ]`) syntax.

**Goal:** Let the level's lamps cast ray-traced shadows from the whole scene while the characters' rig lights keep shadowing only from characters, accept the specular channel in the colour model, and keep the acceleration structure affordable with a cached object-space BLAS for static geometry.

**Architecture:** Per record the shadow pass classifies lights (lamp / rig / specular); the structure has two instance classes with mask bits (static, skinned); static BLASes are built once in object space and referenced by TLAS instances carrying the draw's matrix; skinned draws keep the per-frame view-space BLAS.

**Spec:** `docs/superpowers/specs/2026-10-19-rt-level-shadows-design.md` (read it first: the measurements and the three owner decisions). Earlier plans: `…-ray-tracing-p0-1.md`, `…-p2.md`; spikes `S8`, `S9`.

## Global Constraints

- As phase 2: no guest-side work; RT off or both levels off allocates and records nothing; the budget (High ≈ 100 FPS, ≤ 2.9 ms for all RT work on the RTX 4080, 1440p DLSS Quality) and the FPS targets win over the ms numbers; 1280×720 windowed tests (never fullscreen), few live runs, combat check on demo runs, no real saves; edits with Edit/Write only; build through `build.bat`/`scripts/env.bat`; commits with explicit paths, never the owner's `AGENTS.md`, `scripts/uctest/*`, `tests/test_uctest_autoplay.py`.
- Real-game shapes are the acceptance bar, not fixtures alone (phase 2 lesson: 0 of 3.1 M records were valid until tested on the game's own TEV words and matched live frames). Every classification change adds the game's shapes to a host test; every visible change is checked on a matched on/off pair from the live run (`options` command; pattern in memory `ray-tracing-progress`).
- Clearance and radius constants live in `renderer/rt_shadow_math.h` with tests, tuned on the first scene (1–3 s after the chapter starts, `uctest run perf`) and the fight scene.

## Review Focus

1. **A lamp inside its own fixture** (shade, chandelier): the surface must not go black; the clearance covers the fixture or the ray ends short of it. Task 16.
2. **A mesh drawn with two different matrices in one frame** (the same chair twice, a mirrored copy): each instance gets its own transform and shadows correctly; a mirrored transform does not drop the triangles. Task 17.
3. **A mesh freed while its BLAS is in flight**, and cache churn (a new area loads): no use-after-free, no stall; a missing BLAS means no occlusion for that frame, not a crash. Task 17.
4. **A scene with no skinned draws / no static draws / no lamps:** the pass does not run or leaves the picture alone. Tasks 13, 15.
5. **Characters stay normally lit indoors** with RT on at every level (live pair on the zombie scene). Task 18.

---

## Task 13: Light classes in the shadow pass

**Files:** Modify `renderer/rt_shadow_math.h`, `renderer/shaders/rt_shadow.slang`, `tests/test_rt_shadow_math.cpp`, `tests/renderer_effects_smoke.cpp` (`--rt-shadows`).

**Interfaces:**
- Produces: `enum class RtLightClass { lamp, rig, specular }` and `RtLightClass rt_light_class(const gx::LightUniform&, uint32_t attenuation)` (specular when `attenuation==1`; rig when `|position|² > 1e10` (`classify_light`'s rule); lamp otherwise); `shadow_factor` unchanged. The shader mirrors the classification and traces lamps under `rt_local_shadows`, rigs under `rt_shadows`, specular never (their contribution stays in the total, no ray).
- [ ] **Step 1: Failing tests.** Host: `rt_light_class` of the game's lights — a point light `{pos (0,5,70)}` with attenuation 3 → lamp; a directional `{pos (-5.2e9,6.9e9,4.9e9)}` colour 29 → rig; any light with attenuation 1 → specular; a spot (cosine[1]≠0) → lamp. Smoke `--rt-shadows`: (a) a fixture with one lamp and one rig light on the same record: with `rt_shadows` on and `rt_local_shadows` off only the rig's share leaves (factor = shadow_factor with the lamp's visibility fixed at 1), and the other way round; (b) a specular light never changes the factor.
- [ ] **Step 2: Run** `ctest -R test_rt_shadow_math` and `renderer_effects_smoke --rt-shadows --validation`. Expected: FAIL.
- [ ] **Step 3: Implement** the classification (CPU and shader), the per-class gating.
- [ ] **Step 4: Run** both plus every smoke section and `ctest`. Expected: PASS.
- [ ] **Step 5: Commit.** "Classify the lights of the shadow pass into lamps, rigs and specular".

## Task 14: Channel-1 specular in the colour model

**Files:** Modify `renderer/rt_material.h`, `renderer/scene_lights.h/.cpp` (`ChannelLighting` already has `lit`, `attenuation`), `renderer/vulkan_renderer.cpp` (record fill passes channel 1), `tests/test_rt_material.cpp`, `docs/graphics-effects.md`.

**Interfaces:**
- Produces: `bool light_model_valid(const gx::TevMaterial&, const gx::ChannelLighting& channel0, const gx::ChannelLighting& channel1)`: a stage may read the rasteriser colour of channel 1 (stage `order` channel field 1) when `channel1.lit && channel1.attenuation==1` (the specular function; kind `lit` in the algebra); channel 0 rules unchanged. `kRtSpecular=16` record flag set when a valid draw reads channel 1 (the shader does not use it yet; it documents the approximation).
- [ ] **Step 1: Failing tests** with the game's own shapes: `0x8F8AF` + `0x28F8A0` with stage-1 order `0xC9` (channel 1) and a specular channel 1 → valid; the same with a *diffuse* channel 1 (attenuation 3) → invalid; `0x28F8AF` + `0x8AFF0` (order `0xBF`, `prev + ras1`) with specular channel 1 → valid; ras1 times ras1, or ras1 without a specular channel → invalid; the existing channel-0 cases unchanged.
- [ ] **Step 2: Run** `ctest -R test_rt_material`. Expected: FAIL.
- [ ] **Step 3: Implement** (selector 10 with channel 1 → `lit` when the channel qualifies; any other channel field stays invalid).
- [ ] **Step 4: Measure** with the temporary stats pattern (counter in the record fill, reverted afterwards): the share of valid records in the fight demo and the first scene before and after; record both numbers in the commit message body and in S10.
- [ ] **Step 5: Run** the host and smoke suites. Expected: PASS. **Step 6: Commit.** "Accept the specular channel in the ray-traced shadow colour model".

## Task 15: Static and skinned occluder classes with instance masks

**Files:** Modify `renderer/rt_scene.h/.cpp` (`record_packet` builds up to two BLASes per packet; `build_tlas` writes instance masks), `renderer/shaders/rt_shadow.slang` (ray cull mask per light class), `renderer/vulkan_renderer.cpp` (`record_rt`: static draws included again, classed by `info.indexed`), `tests/renderer_effects_smoke.cpp` (`--rt-as`, `--rt-shadows`), `docs/graphics-effects.md`.

**Interfaces:**
- Consumes: `RtGeometry` (add `bool skinned`), Task 13 classes.
- Produces: instance masks `kRtStatic=1`, `kRtSkinned=2`; `RtScene::Stats` gains `static_blas`, `skinned_blas`; lamp rays use cull mask `0x3`, rig rays `0x2`.
- [ ] **Step 1: Failing smoke:** (a) `--rt-as`: floor (static) + box (skinned) → `static_blas==1`, `skinned_blas==1`, triangles 2 + 6; (b) `--rt-shadows`: a static plate shadows a point lamp's floor point (factor ≈ the CPU reference) and does **not** shadow a rig light's; a skinned plate shadows both; (c) a scene with only static draws and a lamp still traces (the pass runs), a scene with only skinned draws and a lamp traces; a scene without lamps and rigs does nothing.
- [ ] **Step 2: Run** `--rt-as` and `--rt-shadows` with `--validation`. Expected: FAIL.
- [ ] **Step 3: Implement.** Per packet: up to two BLAS builds (one per class), two TLAS instances per packet with the masks and `instanceCustomIndex` = packet index; ray queries take the mask from the light class.
- [ ] **Step 4: Run** all smoke sections and `ctest`. Expected: PASS. **Step 5: Commit.** "Split the ray tracing structure into static and skinned occluders".

## Task 16: Lamp clearance and soft lamp shadows

**Files:** Modify `renderer/rt_shadow_math.h` (`kRtLampClearance`), `renderer/shaders/rt_shadow.slang`, `tests/test_rt_shadow_math.cpp`, smoke `--rt-shadows`, `docs/spikes/S10-rt-level-shadows.md` (new).

- [ ] **Step 1: Failing tests.** Host: `rt_lamp_tmax(distance)` = `max(distance − clearance, 0)`; zero below the clearance (a surface inside the fixture is unshadowed). Smoke: a lamp enclosed by a small static shell (a box around the light, 0.5× the clearance) lights the floor (factor 1), a plate between the lamp and a floor point still shadows it; with the shell larger than the clearance the floor goes dark (documents the limit).
- [ ] **Step 2: Run.** Expected: FAIL. **Step 3: Implement** the clearance (default from the first-scene fixtures: measure the shade extents against the lamp positions from the dump and the captures, record the numbers in S10).
- [ ] **Step 4: Live pair** (first scene and fight scene, `options` toggle): lamps' fixtures do not blacken their surroundings; shadows of seats and tables visible. **Step 5: Commit.** "Stop lamp rays short of their fixtures".

## Task 17: Cached object-space BLAS for static draws

**Files:** Create `renderer/rt_mesh_cache.h/.cpp` (the cache); modify `renderer/rt_scene.h/.cpp` (static instances reference cached BLASes with the draw's 3×4 transform), `renderer/vulkan_renderer.h/.cpp` (mesh identity and slab lifetime hooks, `record_rt`), `tests/test_rt_mesh_cache.cpp` (host: policy), `tests/renderer_effects_smoke.cpp` (`--rt-cache`), `docs/graphics-effects.md`.

**Interfaces:**
- Produces: `class RtMeshCache { std::optional<VkDeviceAddress> blas_for(MeshKey, build_request) ...; void evict_slab(const void* slab); Stats stats() const; }` where `MeshKey` = (slab pointer, vertex offset, index offset, index count); a miss schedules a build (at most `kBuildsPerFrame` per frame, 4 by default) and the draw does not occlude until it exists; a byte budget (`kMeshCacheBytes`, 256 MiB by default) with least-recently-used eviction; the pure policy (key, budget, LRU, per-frame build cap) is a header-only class with host tests.
- Static instance transform: the draw's palette row (`current` row of its `PaletteBlock`: rows `current..current+2`) as the TLAS instance's row-major 3×4; mirrored matrices flip winding (ray flags `RAY_FLAG_NONE`, no facing cull).
- [ ] **Step 1: Failing host tests** for the policy: a hit returns the cached BLAS; a miss under the per-frame cap schedules a build, beyond the cap defers; adding past the byte budget evicts the least recently used; evicting a slab drops its meshes; the same mesh under two keys never aliases.
- [ ] **Step 2: Failing smoke `--rt-cache`:** (a) the same box mesh drawn twice with different matrices (one mirrored) → one cached BLAS, two instances, both cast the CPU-reference shadow; (b) frame two with the camera moved (a different matrix) → no BLAS build (`stats().builds` unchanged), shadow still right; (c) a mesh freed (slab released) → its BLAS destroyed after its frames complete, no validation error; (d) `--validation` clean, no per-frame allocation after warm-up.
- [ ] **Step 3: Run** both. Expected: FAIL.
- [ ] **Step 4: Implement** the policy class, the GPU cache (object-space BLAS from the mesh's slab vertex/index buffers, `PREFER_FAST_TRACE`, built in the frame's command buffer, retired behind the slot fence), instance records, eviction hooks.
- [ ] **Step 5: Run** all smoke sections, `ctest`. Expected: PASS. **Step 6: Commit.** "Cache object-space BLASes of static meshes for ray-traced shadows".

## Task 18: Measure, live check, budgets, deploy

**Files:** Modify `renderer/rt_shadow_math.h` (presets only if a budget is missed), `docs/spikes/S10-rt-level-shadows.md`, `docs/graphics-effects.md`.

- [ ] **Step 1: Cost.** The fight3 demo per setting as in S9 (off, low, medium, high, ultra) plus the first-scene perf run: `interval_ms`, `gpu_rt_as_ms`, `gpu_rt_shadows_ms`, `rt_*` stats; combat check; one run each. Required: High end-to-end RT cost ≤ the S9 number (+1.06 ms) after the cache, or the spike lists what is left and why.
- [ ] **Step 2: Live pairs** (matched on/off with the `options` command): the dining car floor and tables under the lamps, the zombie scene, a corridor; read the frames, describe the shadows, confirm no black characters, no lamp blackouts, no shimmer in three frames 100 ms apart.
- [ ] **Step 3: Update** the docs (design rules, options' meaning: "lamps", "character rig lights", the specular approximation). **Step 4: Full build** (`build.bat`, deploy after moving test leftovers out of `dist`), hand over for the owner's check. **Step 5: Commit** "Measure ray-traced level shadows (S10)".

## Task 19: Final review and fix pass

- [ ] One fresh review of the phase on the strongest model with this plan's Review Focus; fix Critical/Important findings test-first in one pass (the phase 2 routine); ledger rulings and deferred minors; merge decision stays the owner's.

## Order and dependencies

13 → 14 independent of 15; 15 before 17 (the cache needs the static class); 16 after 15; 18 after all. Tasks 13 and 14 can land first and already improve the picture on characters (class gating, more valid records).
