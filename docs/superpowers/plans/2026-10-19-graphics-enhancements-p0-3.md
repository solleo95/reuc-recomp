# Graphics enhancements, phases 0–3 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Survey the game's lighting, then ship anisotropic filtering, SMAA, per-pixel lighting, the normal G-buffer, GTAO and contact shadows as separate options in the launcher and the in-game menu.

**Architecture:** Every effect lives in the renderer on the main host thread and the GPU; the guest threads are never touched. Screen-space effects run in compute on the internal scene targets at the scene boundary, before the upscale (`VulkanRenderer::record_upscale`). That path is generalised to run with SMAA or with no upscaler (blit pass-through at ratio 1). The G-buffer is a third attachment of the existing motion pipeline set. Per-pixel lighting is a push-constant flag in the same shaders.

**Tech Stack:** C++20, Vulkan 1.3 (dynamic rendering, `VK_EXT_dynamic_rendering_unused_attachments`), Slang → SPIR-V (`slangc`), ImGui (launcher and menu), nlohmann::json, Python `uctest` harness.

**Spec:** `docs/superpowers/specs/2026-10-19-graphics-enhancements-design.md`

## Global Constraints

- No work, lock or wait is added on the guest side (scheduler threads, `gx::Packets` producer). All new code runs in the renderer or on renderer-owned worker threads.
- With every effect off and anisotropy 1, the image equals today's on the current path, pixel for pixel.
- With effects off, the deterministic bench must not get slower by more than 1% (`python -m uctest bench <before> <after> --demo fight2 --runs 3`).
- Effects apply only to the draws before the scene boundary in a frame that has a scene. HUD, menus, movies, black frames and the post chain are untouched.
- Defaults: `graphics.anisotropy` 16; every other effect off.
- Options 3–6 need the motion path (`VK_EXT_dynamic_rendering_unused_attachments` + `independentBlend`). Without it they are greyed out with a reason; anisotropy and SMAA still work.
- Budgets per effect (GPU ms, pass timestamps). low: ≤ 0.5 ms on an RTX 3060 at 1080p. medium: ≤ 1.0 ms on an RTX 3060 at 1080p. high: ≤ 1.0 ms on an RTX 4080 at 1440p. ultra: ≤ 2.0 ms on an RTX 4080 at 4K with DLSS quality. On the owner's RTX 4080 at 1080p, low is checked at ≤ 0.2 ms and medium at ≤ 0.4 ms.
- Every user-visible string has all 13 columns in `launcher/translations.inc` (en, ru, de, fr, es, it, pt-BR, pl, uk, tr, ja, ko, zh-Hans). The launcher and the menu share the keys.
- Live testing goes only through the harness (`.claude/skills/uc-test/SKILL.md`), never system input or real saves. Runs are unlocked (`--fps-limit 0`). Use one bench run per question and one or two live runs per feature.
- Build: `build.bat --no-install`. Tests: `ctest --test-dir build --output-on-failure`.
- Source edits only through Edit/Write (no heredoc, sed or python patches). Check each changed line that contains an escape sequence before the first build.
- Commits as listed per task. Messages carry no AI attribution. The working tree's unrelated uncommitted changes (in-game menu, upscaler runtime work) must be committed by the owner before Task 1. The executor asks the owner before the first commit.

## Review Focus

1. **A camera cut or a scene/no-scene switch while AO or contact shadows are on.** Expect no smear from the previous frame and no darkening of menus or movies. Covered in Task 12 (smoke: frame without scene → effect passes not recorded).
2. **A frame whose scene has no lit draw and no light.** Expect contact shadows to do nothing and nothing to crash. Covered in Task 11 (`select_dominant` on an empty list) and Task 13 (smoke: no light → output unchanged).
3. **Changing render scale or upscaler while an effect is on.** Expect the effect's targets to be recreated at the new size with no validation error. Covered in Task 10 (smoke resizes with the path on and runs `--validation`).
4. **A GPU without unused-attachment support, or with `maxSamplerAnisotropy` < 16.** Expect the options to be greyed out or clamped, and the game to start. Covered in Task 3 (feature report fields) and Task 4 (clamp test).
5. **The menu toggles an effect every frame for a second.** Expect no device rebuild per toggle except for scene-path entry and exit, and no leak. Covered in Task 12 (smoke toggles AO 60 times and checks `descriptor_pool_creations()` and `texture_creations()` are stable after the first).

---

## Phase 0 — Survey

### Task 1: Light classification, lighting stats and a callsite skip switch

**Files:**
- Create: `renderer/scene_lights.h`, `renderer/scene_lights.cpp`
- Modify: `renderer/CMakeLists.txt` (add `scene_lights.cpp` to `uc_gx`)
- Modify: `game/packet_dump.h` (new `UC_LIGHT_STATS` section)
- Modify: `renderer/vulkan_renderer.cpp:1528` (next to the bloom skip)
- Test: `tests/test_scene_lights.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `enum class gx::LightKind { directional, point, spot, specular };`
  - `struct gx::ChannelLighting { bool lit=false; uint8_t mask=0; uint32_t attenuation=0; uint32_t diffuse=0; };`. The fields decode a GX channel control word as `lightControl` in `smoke.slang` does: `lit` = bit 1; `mask` = `((c>>2)&15)|((c>>7)&240)`; `diffuse` = `(c>>7)&3`; `attenuation` = `(c>>9)&3`.
  - `gx::ChannelLighting gx::channel_lighting(uint32_t control);`
  - `gx::LightKind gx::classify_light(const gx::LightUniform& light, uint32_t attenuation);`. The rules: `attenuation==1` → specular. Otherwise, a position length above 1e5 → directional. Otherwise `attenuation==3` with `cosine[1]!=0 || cosine[2]!=0` → spot. Everything else → point.
  - Env `UC_SKIP_CALLSITES=<hex>[,<hex>...]` (debug): draws with a listed callsite are not recorded.

- [ ] **Step 1: Write the failing test** `tests/test_scene_lights.cpp`. Use `CHECK` from `tests/check.h`.
  - `channel_lighting(0x2 | (1<<2) | (2<<7) | (3<<9))` gives `lit`, `mask==1`, `diffuse==2`, `attenuation==3`.
  - `channel_lighting(0x1).lit==false`.
  - A light with `position={1e6,0,0}` and attenuation 0 → `directional`.
  - `position={1,2,3}`, attenuation 3, `cosine={1,0,0}` → `point`.
  - The same light with `cosine={-3,4,0}` → `spot`.
  - Attenuation 1 → `specular`.
  - Register `test_scene_lights` linked to `uc_gx`, the same way `test_post_effects` is.
- [ ] **Step 2: Run it.** Command: `ctest --test-dir build -R test_scene_lights --output-on-failure` after `build.bat --no-install`. Expected: FAIL (does not compile: `scene_lights.h` missing).
- [ ] **Step 3: Implement** `channel_lighting` and `classify_light` in `renderer/scene_lights.cpp`.
- [ ] **Step 4: Run it.** Same command. Expected: PASS.
- [ ] **Step 5: Add the `UC_LIGHT_STATS=<file>` section** to `PacketDump`, following the `UC_PACKET_STATS` pattern (`enable_stats`, written in the destructor). The section covers scene draws only (draws before `SceneBoundary`'s cut; `PacketDump` already tracks the scene through `enable_scene`).
  - Key each row by `callsite`. Columns:
    - draws
    - vertices
    - `lit0` (channel 0 lit)
    - `lit1`
    - light kinds used (counts per `LightKind` over the mask's lights)
    - has normals (mesh vertices with a non-zero `object_normal`)
    - blend mode/src/dst
    - alpha compare active (`pixel[0]` not "always")
    - depth write (`pixel[1]` bit 4)
    - texture count
  - Write one CSV line per callsite, sorted by vertices descending.
- [ ] **Step 6: Add `UC_SKIP_CALLSITES`**, parsed once (static) into a `std::vector<uint32_t>`. In `record_pass`, `continue` for listed callsites, next to `if (!bloom_ && gx::bloom_composite(draw)) continue;`.
- [ ] **Step 7: Build and run all tests.** Command: `build.bat --no-install`, then `ctest --test-dir build --output-on-failure`. Expected: all PASS.
- [ ] **Step 8: Commit.** Message: "Add GX light classification, scene lighting stats and a callsite skip switch for the graphics survey".

### Task 2: Survey runs and the survey report

**Files:**
- Create: `docs/spikes/S6-graphics-survey.md`

**Interfaces:**
- Consumes: `UC_LIGHT_STATS`, `UC_PACKET_STATS`, `UC_SKIP_CALLSITES` (Task 1); harness scenarios `train_gameplay`, `fight`.
- Produces: the report the phase 4–6 plan is written from. Its numbers are quoted there.

- [ ] **Step 1: Capture the stats in gameplay.** Set `UC_LIGHT_STATS=build/survey/train-lights.csv` and `UC_PACKET_STATS=build/survey/train-packets.csv` in the environment, then run `cd scripts && python -m uctest run train_gameplay --speed 8`. Expected: both CSVs exist and the lights file has more than 20 callsites.
- [ ] **Step 2: Capture the stats in the fight.** Same environment with `fight-*.csv` names, then run `python -m uctest run fight --speed 8`.
- [ ] **Step 3: Find the game's own shadows.** Candidates are scene draws with multiplicative or reverse-subtract blend (`blend.mode==3`, or destination `SRC_COLOR`/source `DST_COLOR`) and no lighting, drawn after the floor. Confirm each candidate with one capture run with `UC_SKIP_CALLSITES=<candidate>` and frame captures (uc-test skill, capture section). Compare against a run without the skip.
- [ ] **Step 4: Write `docs/spikes/S6-graphics-survey.md`.** Answer each with numbers from the CSVs:
  - the share of scene vertices that are GX-lit versus baked colour;
  - the light kinds and their counts per frame;
  - whether a stable dominant directional or spot light exists in the train and in the fight;
  - the game's shadow callsites, or "none";
  - opaque versus alpha-tested versus blended vertex share;
  - normal coverage;
  - the number of meshes eligible for tessellation (lit, triangles, opaque, with normals).

  End with one line per phase 4–6 feature: "go", "go with change X" or "drop", with the reason.
- [ ] **Step 5: Commit.** Message: "Add the graphics survey (lighting, game shadows, normals) for the phase 4-6 plan".

---

## Phase 1 — Anisotropy, SMAA, effects plumbing

### Task 3: The effects block in options, config, CLI, live settings and the feature report

**Files:**
- Create: `renderer/graphics_effects.h` (header-only)
- Modify: `game/options.h`, `game/options.cpp`, `game/options_config.cpp`, `game/live_settings.h`, `game/live_settings.cpp`, `game/uc_main.cpp` (usage text, `SettingsTargets` wiring, `--features` report), `game/feature_report.h`, `renderer/upscaler_probe.h`, `renderer/upscaler_probe.cpp`
- Test: `tests/test_options.cpp`, `tests/test_options_config.cpp`, `tests/test_live_settings.cpp`, `tests/test_feature_report.cpp`

**Interfaces:**
- Produces:
  - `struct uc::GraphicsEffects { uint32_t anisotropy=16; bool operator==(const GraphicsEffects&) const=default; };` in `renderer/graphics_effects.h`. Later tasks add one field each.
  - `bool uc::valid_anisotropy(uint32_t)`: true for 1, 2, 4, 8 and 16.
  - `uc::Options::effects`, of type `GraphicsEffects`.
  - Config `graphics.anisotropy` (number). CLI `--anisotropy N`. An invalid value is the error `"invalid value for --anisotropy"`, and in config `"graphics.anisotropy must be 1, 2, 4, 8 or 16"`.
  - `uc::SettingsTargets::effects`, of type `std::function<void(const GraphicsEffects&)>`. `LiveSettings::apply` calls it once when `effects` changed.
  - `FeatureReport` adds:
    - `bool effects` (motion path available);
    - `std::string effects_reason`;
    - `uint32_t anisotropy_max` (1 when `samplerAnisotropy` is unsupported).

    JSON keys `effects`, `effects_reason`, `anisotropy_max`. An old report without these keys reads as `effects=false`, `anisotropy_max=1`.
  - `UpscalerSupport` gains the same three fields, filled in `probe_upscalers` from `VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT`, `independentBlend`, `samplerAnisotropy` and `limits.maxSamplerAnisotropy` (rounded down to a power of two, at most 16).

- [ ] **Step 1: Write the failing tests.**
  - `test_options`:
    - `--anisotropy 8` gives `effects.anisotropy==8`;
    - `--anisotropy 3` gives the error string;
    - the default is 16.
  - `test_options_config`:
    - `{"graphics":{"anisotropy":4}}` loads 4;
    - saving writes `graphics.anisotropy`;
    - `{"graphics":{"anisotropy":5}}` is rejected with the message above.
  - `test_live_settings`: changing only `effects.anisotropy` gives `calls=={"effects"}`, and the saved file has it.
  - `test_feature_report`:
    - a round trip keeps `effects`, `effects_reason` and `anisotropy_max`;
    - parsing `{"dlss":true}` gives `effects==false` and `anisotropy_max==1`.
- [ ] **Step 2: Run them.** Command: `ctest --test-dir build -R "test_options|test_live_settings|test_feature_report" --output-on-failure`. Expected: FAIL (compile errors on the missing fields).
- [ ] **Step 3: Implement** the header, the `Options` field, parsing (next to `--upscale-mode` in `options.cpp`), config load and save (next to `bloom` in `options_config.cpp`), the `LiveSettings` diff and target, the `uc_main.cpp` wiring (`targets.effects=[&](const GraphicsEffects& e){ options.effects=e; renderer.set_effects(e); }`; `set_effects` comes in Task 4, so stub the call out until then), the report fields and the probe.
- [ ] **Step 4: Run all tests.** Command: `ctest --test-dir build --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit.** Message: "Add the graphics effects block with the anisotropy setting and the effects capability report".

### Task 4: Anisotropic filtering in the renderer

**Files:**
- Modify: `renderer/vulkan_renderer.h`, `renderer/vulkan_renderer.cpp:216-224` (device features), `:516-532` (samplers)
- Modify: `game/uc_main.cpp` (`renderer.set_effects(options.effects)` before the first frame; replace the Task 3 stub)
- Test: `tests/renderer_smoke.cpp` (new case)

**Interfaces:**
- Consumes: `GraphicsEffects` (Task 3).
- Produces:
  - `void VulkanRenderer::set_effects(const GraphicsEffects&)`. It can be called before or after `initialize`, and the change applies at the next `render()` call. A change of `anisotropy` waits for the device to go idle (`present_.wait_idle(device)`, as `set_upscaling` does), destroys `texture_samplers_` and `scene_samplers_`, and rebuilds them.
  - `uint32_t VulkanRenderer::effective_anisotropy() const`: `min(requested, device max)`, or 1 without `samplerAnisotropy`.
  - `static uint32_t uc::clamp_anisotropy(uint32_t requested, bool supported, float device_max)` in `graphics_effects.h`, which `effective_anisotropy` uses.

- [ ] **Step 1: Write the failing tests.**
  - Add to `test_options.cpp` (pure function): `clamp_anisotropy(16,true,8.f)==8`, `clamp_anisotropy(16,false,16.f)==1`, `clamp_anisotropy(4,true,16.f)==4`.
  - Add a `renderer_smoke` case: a textured floor quad at a grazing angle (mipmapped checker, linear min/mip filter), rendered with anisotropy 1 and then 16. Assert that a pixel band at 80% of the screen height has a higher standard deviation with 16 (more detail survives) than with 1. Then render the HUD quad (nearest filter) with 1 and 16, and assert both are identical.
- [ ] **Step 2: Run it.** Command: `ctest --test-dir build -R "renderer_smoke|test_options" --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.**
  - Enable `samplerAnisotropy` in `enabled` when `core.samplerAnisotropy` is set.
  - In the sampler loop, set `anisotropyEnable`/`maxAnisotropy` only when `minFilter` is linear and `mip==2` (linear mipmaps), and only for `scene_samplers_`. On the no-upscaler path (no `scene_samplers_`), apply it to `texture_samplers_` with the same filter condition. Nearest-filtered samplers keep it off, which keeps HUD and 2D draws unchanged.
- [ ] **Step 4: Run it.** Same command. Expected: PASS. Also run `build\renderer_smoke.exe --validation`: no validation errors.
- [ ] **Step 5: Commit.** Message: "Add anisotropic filtering for mipmapped scene textures".

### Task 5: The Effects group in the launcher and the Effects tab in the in-game menu

**Files:**
- Modify: `launcher/main.cpp` (graphics page, after the frame generation block, before bloom)
- Modify: `game/settings_menu.h`, `game/settings_menu.cpp` (third tab `effects_tab(float unit)`)
- Modify: `launcher/translations.inc`
- Test: `tests/test_settings_menu.cpp`

**Interfaces:**
- Consumes: `Options::effects`, `FeatureReport::anisotropy_max`, `FeatureReport::effects` (Task 3).
- Produces:
  - Translation keys:
    - `effects` ("Effects" / "Эффекты");
    - `anisotropy` ("Anisotropic filtering" / "Анизотропная фильтрация");
    - `effects_unavailable` ("This GPU cannot run screen-space effects" / "Эта видеокарта не поддерживает экранные эффекты").
  - The pattern later tasks extend: one `ImGui::TextUnformatted(tr(key))` label and one combo or checkbox per option, using the tab's existing widgets (`BeginCombo`/`Selectable` in the menu, `combo`/`toggle`/`track` in the launcher).
  - Anisotropy combo values: `1×` (labelled with the `off` key), `2×`, `4×`, `8×`, `16×`. Values above `anisotropy_max` are disabled, with a tooltip `unsupported`.

- [ ] **Step 1: Write the failing test.** In `tests/test_settings_menu.cpp`, change `CHECK(tabs->Tabs.Size==2)` to `==3`. Add: select the third tab through ImGui input, find the `##anisotropy` combo window, open it, pick `4×`, call `menu.commit()`, and check `live.options().effects.anisotropy==4`.
- [ ] **Step 2: Run it.** Command: `ctest --test-dir build -R test_settings_menu --output-on-failure`. Expected: FAIL (2 tabs).
- [ ] **Step 3: Implement** the tab, the launcher group and the translations (13 columns).
- [ ] **Step 4: Run it.** Same command. Expected: PASS. Then open the launcher once (`dist\launcher.exe`) and check the Effects group looks like the rest of the graphics page.
- [ ] **Step 5: Commit.** Message: "Add the Effects group to the launcher and the Effects tab to the in-game menu".

### Task 6: GPU pass timestamps

**Files:**
- Create: `renderer/pass_timer.h`, `renderer/pass_timer.cpp`
- Modify: `renderer/vulkan_renderer.h/.cpp` (owns a `PassTimer`; `record_upscale` brackets the upscaler call), `game/frame_log.h/.cpp` (columns), `game/uc_main.cpp` (`--profile` line)
- Test: `tests/renderer_upscale_smoke.cpp`

**Interfaces:**
- Produces:
  - `enum class uc::GpuPass : uint32_t { upscale, smaa, occlusion, count };`. Later phases append to it.
  - `class uc::PassTimer` with:
    - `initialize(VkDevice, float timestamp_period, uint32_t slots)`;
    - `begin(VkCommandBuffer, uint32_t slot, GpuPass)`;
    - `end(VkCommandBuffer, uint32_t slot, GpuPass)`;
    - `void collect(uint32_t slot)` (called where the slot's fence was seen complete);
    - `double last_ms(GpuPass) const`;
    - `shutdown()`.

    It uses one query pool of `slots*count*2` timestamps, and resets each slot's queries at its first `begin` of a submission.
  - `VulkanRenderer::pass_ms(GpuPass) const`.
  - Frame log columns `gpu_upscale_ms`, `gpu_smaa_ms`, `gpu_occlusion_ms`. The `--profile` line adds `gpu passes: upscale X ms, smaa Y ms, occlusion Z ms`.

- [ ] **Step 1: Write the failing test.** After the DLSS-free blit run in `renderer_upscale_smoke`, assert `renderer.pass_ms(uc::GpuPass::upscale)>0`, and that `pass_ms` of a pass that never ran is 0.
- [ ] **Step 2: Run it.** Command: `build\renderer_upscale_smoke.exe`. Expected: FAIL (compile).
- [ ] **Step 3: Implement** `PassTimer` and the wiring.
- [ ] **Step 4: Run it.** Same command plus `--validation`. Expected: PASS, no validation errors.
- [ ] **Step 5: Commit.** Message: "Add per-pass GPU timestamps to the frame log and profile output".

### Task 7: SMAA as an anti-aliasing choice

**Files:**
- Create: `third_party/smaa/` (`SMAA.hlsl`, `AreaTex.h`, `SearchTex.h`, `LICENSE`, from iryoku/smaa at a pinned commit; record the commit and SHA256 of each file in `THIRD_PARTY_NOTICES.md`)
- Create: `renderer/shaders/smaa.slang` (three entry points: `edgesMain`, `weightsMain`, `blendMain`; it includes `SMAA.hlsl` with `SMAA_PRESET_HIGH` and the Slang/HLSL mapping macros)
- Create: `renderer/smaa_upscaler.cpp`
- Modify: `renderer/upscale_mode.h` (`UpscalerKind { off, dlss, fsr, blit, smaa }`; appended so stored indices stay stable), `renderer/upscaler.h` (`make_smaa_upscaler`), `renderer/CMakeLists.txt` (shader build and source), `renderer/vulkan_renderer.cpp:1125-1133` (create it), `:528-531` (mip bias 0 for SMAA), `:1240` (jitter 0 unless DLSS/FSR)
- Modify: `game/options.cpp` (`kUpscalerNames` gets `"smaa"`, valid in config and CLI), `game/live_settings.cpp` (`with_upscaler(smaa)` turns frame generation off), `game/settings_menu.cpp`, `launcher/main.cpp` (`smaa` entry in the upscaling combo; the quality and frame generation rows are hidden for SMAA), `launcher/translations.inc` (`upscaling` label becomes "Anti-aliasing / upscaling" / "Сглаживание / масштабирование")
- Test: `tests/test_options.cpp`, `tests/test_live_settings.cpp`, `tests/renderer_upscale_smoke.cpp`

**Interfaces:**
- Consumes: `Upscaler` interface (`renderer/upscaler.h`), `PassTimer` (`GpuPass::smaa`).
- Produces:
  - `std::unique_ptr<uc::Upscaler> uc::make_smaa_upscaler(VkDevice, const VkPhysicalDeviceMemoryProperties&, const std::filesystem::path& shaders);`. Its `minimum_internal(output)` returns `output`, so the scene runs at ratio 1 regardless of `upscale_mode`, and `sharpens()` returns false (RCAS still applies when sharpening is on).
  - `bool uc::uses_jitter(UpscalerKind)`: true for dlss and fsr only.

- [ ] **Step 1: Write the failing tests.**
  - `test_options`: `--upscaler smaa` parses, and config `"upscaler":"smaa"` loads.
  - `test_live_settings`: `with_upscaler(options_with_fg, UpscalerKind::smaa).frame_generation==false`.
  - `renderer_upscale_smoke --smaa`: render the existing still edge scene with SMAA.
    - The count of distinct intermediate values along the diagonal edge is ≥ 3. The upscaler-off count is ≤ 2 (hard edge).
    - A flat-colour region is identical to off.
    - Jitter is zero: two consecutive frames are identical.
    - The scene extent equals the EFB extent.
- [ ] **Step 2: Run them.** Command: `ctest --test-dir build -R "test_options|test_live_settings" --output-on-failure` and `build\renderer_upscale_smoke.exe --smaa`. Expected: FAIL.
- [ ] **Step 3: Implement.**
  - The upscaler runs three fullscreen compute dispatches over `render_size`:
    - luma edge detection into an RG8 edges image;
    - blend weights into RGBA8, with the area and search textures uploaded once at `initialize`;
    - neighbourhood blending from `in.color` into `in.output`.
  - Its images are created on the first `evaluate` and on size change. They stay in GENERAL, as the `Upscaler` contract requires.
  - Timestamps: `GpuPass::smaa` around the three passes.
- [ ] **Step 4: Run them.** Same commands plus `--validation`. Expected: PASS, no validation errors.
- [ ] **Step 5: Check the cost and the image live.**
  - Bench (one run): build the baseline before this task as `dist/uc_before.exe` and the new exe as `dist/uc_smaa.exe`, then run `python -m uctest bench dist/uc_before.exe dist/uc_smaa.exe --demo fight2 --game-arg=--upscaler --game-arg=off`. Expected: CI within ±1% (off path unchanged).
  - Live: `python -m uctest run fight --game-arg=--upscaler --game-arg=smaa --game-arg=--profile`. Expected: `smaa` ≤ 0.4 ms at 1080p on the RTX 4080, and the captured frames show smoothed edges and a sharp HUD.
- [ ] **Step 6: Commit.** Message: "Add SMAA 1x as an anti-aliasing choice next to DLSS and FSR".

---

## Phase 2 — Per-pixel lighting and the G-buffer

### Task 8: Per-pixel lighting

**Files:**
- Modify: `renderer/shaders/smoke.slang` (MOTION variant).
  - VS outputs `rawColor0` (location 12), `rawColor1` (13), `viewPosition` (14) and `viewNormal` (15).
  - `Projection` gains `uint effects` and `uint pad[3]`.
  - When `(projection.effects & 1) != 0`, `shadeFragment` replaces the rasterised `COLOR0`/`COLOR1` with `shadeChannel(rawColorN, N, viewPosition, normalize(viewNormal))` for channels whose control word has bit 1 (lighting) set. Channels without lighting keep the interpolated value.
- Modify: `renderer/vulkan_renderer.cpp:273-274` (`DrawPush` gets `uint32_t effects=0, pad[3]{}`; `static_assert(sizeof(DrawPush)==128)`), `:1553` (set `push.effects` for scene draws when `per_pixel_lighting` is on and `motion_outputs_`)
- Modify: `renderer/graphics_effects.h` (`bool per_pixel_lighting=false;`), `game/options.cpp` (`--per-pixel-lighting`), `game/options_config.cpp` (`graphics.per_pixel_lighting`), `game/settings_menu.cpp`, `launcher/main.cpp` (checkbox, disabled when `!features.effects` with tooltip `effects_unavailable`), `launcher/translations.inc` (`per_pixel_lighting`: "Per-pixel lighting" / "Попиксельное освещение")
- Test: `tests/renderer_smoke.cpp`, `tests/test_options.cpp`, `tests/test_options_config.cpp`

**Interfaces:**
- Consumes: `GraphicsEffects`, `set_effects` (Tasks 3–4).
- Produces:
  - `constexpr uint32_t uc::kEffectPerPixelLighting=1;` in `graphics_effects.h`. Later push-constant bits use the next values.
  - `GraphicsEffects::per_pixel_lighting`.

- [ ] **Step 1: Write the failing tests.**
  - `test_options` / `test_options_config`: the flag and the config key round trip, and the default is false.
  - `renderer_smoke` "per-pixel lighting": a 2-triangle quad facing the camera, lighting channel 0 on, a point light (attenuation 3, distance `{1,0,0.5}`) 0.2 units in front of the quad centre, and vertex colour white. Render with the flag off and on.
    - Centre pixel (on) minus centre pixel (off) ≥ 40 levels.
    - The corner pixels differ by ≤ 1 level.
    - A second draw with lighting off (baked colour) is identical between off and on.
    - Toggling does not create pipelines: compare the pipeline count before and after. Add `size_t pipeline_count() const` to the renderer for tests, returning `pipelines_.size()+uber_pipelines_.size()` under the mutex.
- [ ] **Step 2: Run them.** Command: `ctest --test-dir build -R "renderer_smoke|test_options" --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement** the shader, push and option changes, and the menu and launcher checkbox.
- [ ] **Step 4: Run them.** Same command plus `renderer_smoke --validation`. Expected: PASS.
- [ ] **Step 5: Bench the off path.** One run: `python -m uctest bench dist/uc_before.exe dist/uc_ppl.exe --demo fight2`. Expected: the CI includes 0 or shows ≤ 1% slower. If it is slower than 1%, stop and report: the extra interpolants cost too much with effects off.
- [ ] **Step 6: Check it live.** Run `python -m uctest run train_gameplay --game-arg=--per-pixel-lighting` and capture frames. Compare against an off capture at the same mark, and show both to the owner.
- [ ] **Step 7: Commit.** Message: "Add per-pixel GX lighting as a live push-constant option".

### Task 9: The normal G-buffer

**Files:**
- Create: `renderer/gbuffer.h` (C++), `renderer/shaders/gbuffer.slang` (the same encode and decode)
- Modify: `renderer/shaders/smoke.slang`.
  - `Targets` gains `float4 gbuffer : SV_Target2` (MOTION only).
  - The FS writes `encode_gbuffer(viewNormal, fog, flags)`. `applyFog` is refactored to return its 0..1 factor alongside the colour.
- Modify: `renderer/vulkan_renderer.h/.cpp`:
  - `kGBufferFormat = VK_FORMAT_R16G16B16A16_UNORM`;
  - `scene_gbuffer_` image, memory and view, created and destroyed with the other scene targets;
  - `formats[3]` and `colorAttachmentCount = motion_outputs_ ? 3 : 1` in `make_pipeline` and `record_pass`;
  - `blends[2].colorWriteMask = gbuffer_write_mask(key)` (RGBA when depth write is on and blend mode is 0, otherwise 0);
  - the G-buffer bound in scene passes only when `effects_need_gbuffer()`, cleared once per frame like motion;
  - `PassTarget` gains `gbuffer_view`.
- Modify: `renderer/pipeline_keys.h` (declare `gbuffer_write_mask(const PipelineKey&)` next to `motion_write_mask`)
- Test: `tests/test_gbuffer.cpp`, `tests/test_pipeline_keys.cpp`, `tests/renderer_upscale_smoke.cpp`

**Interfaces:**
- Produces:
  - `std::array<uint16_t,4> uc::encode_gbuffer(std::array<float,3> view_normal, float fog, uint16_t flags);`
  - `struct uc::GBufferTexel { std::array<float,3> normal; float fog; uint16_t flags; };`
  - `uc::GBufferTexel uc::decode_gbuffer(std::array<uint16_t,4>);`. Octahedral normal in .xy (unorm16), fog in .z, flags in .w.
  - Flags: `kGBufferHasNormal=1`, `kGBufferLit=2`, `kGBufferTessellated=4` (reserved), `kGBufferReflective=8` (reserved). A cleared texel (all 0) means "no surface".
  - `VulkanRenderer::effects_need_gbuffer() const`: false in this task. Task 12 makes it return true when AO or contact shadows are on.
  - `RenderPixels`-like test capture: `struct GBufferPixels { uint32_t width, height; std::vector<uint16_t> texels; };` and `GBufferPixels VulkanRenderer::capture_gbuffer()` (waits for the GPU), like `capture_motion`.
  - Test hook `void VulkanRenderer::force_gbuffer(bool)` (smoke tests only).

- [ ] **Step 1: Write the failing tests.**
  - `test_gbuffer`:
    - encode then decode of `{0,0,1}`, `{0.6,0,0.8}` and `{-0.577,-0.577,-0.577}` keeps the normal within 0.001;
    - fog 0.25 is kept within 1/65535;
    - flags round trip exactly;
    - `decode_gbuffer({0,0,0,0}).flags==0`.
  - `test_pipeline_keys`: `gbuffer_write_mask` is 0xF for an opaque depth-writing key and 0 for a blended key.
  - `renderer_upscale_smoke --gbuffer` (blit upscaler, `force_gbuffer(true)`): a lit opaque quad facing the camera, a blended quad over half of it, and an empty background.
    - The lit pixels decode to normal z > 0.99 with flags `HasNormal|Lit`.
    - Under the blended quad the texel is unchanged (still the opaque quad's).
    - The background has flags 0.
    - After `set_render_scale` the capture has the new size and validation stays clean.
- [ ] **Step 2: Run them.** Command: `ctest --test-dir build -R "test_gbuffer|test_pipeline_keys" --output-on-failure` and `build\renderer_upscale_smoke.exe --gbuffer --validation`. Expected: FAIL.
- [ ] **Step 3: Implement** the encode and decode (C++ and Slang must match: octahedral wrap per Cigolle et al. 2014), the attachment, the masks and the capture.
- [ ] **Step 4: Run them.** Same commands. Expected: PASS, no validation errors.
- [ ] **Step 5: Bench the off path** (G-buffer unbound). One run, as in Task 8 Step 5, same 1% rule.
- [ ] **Step 6: Commit.** Message: "Add the normal G-buffer as a third scene attachment".

---

## Phase 3 — Screen-space occlusion

### Task 10: The scene-target path without an upscaler

**Files:**
- Modify: `renderer/graphics_effects.h` (`bool needs_scene_targets(const GraphicsEffects&)`)
- Modify: `renderer/vulkan_renderer.cpp`:
  - `:1125-1146` in `rebuild()`: when `upscaler_kind_==off` and `needs_scene_targets(effects_)` and `motion_outputs_`, create `make_blit_upscaler()` with internal extent = EFB extent;
  - `set_effects`: when `needs_scene_targets` flips, set `resized_=true` so `rebuild()` creates or drops the pass-through, after `present_.wait_idle`;
  - the mip bias stays 0 for the pass-through;
  - jitter only when `uses_jitter`.
- Test: `tests/renderer_upscale_smoke.cpp`

**Interfaces:**
- Consumes: `uses_jitter` (Task 7).
- Produces: `bool uc::needs_scene_targets(const GraphicsEffects&)`, which returns false in this task. Task 12 makes it true for `ambient_occlusion!=off || contact_shadows`. The smoke test drives the path through the test hook `force_scene_targets(bool)` until then.

- [ ] **Step 1: Write the failing test.** `renderer_upscale_smoke --passthrough` renders the existing still scene with no upscaler, once with `force_scene_targets(false)` and once with `true`.
  - The two EFB captures are identical byte for byte.
  - `upscale_internal_extent()` equals the EFB size with the path on and {0,0} with it off.
  - Toggling 10 times keeps `descriptor_pool_creations()` constant after the first toggle.
- [ ] **Step 2: Run it.** Command: `build\renderer_upscale_smoke.exe --passthrough --validation`. Expected: FAIL.
- [ ] **Step 3: Implement** the path change.
- [ ] **Step 4: Run it.** Same command. Expected: PASS.
- [ ] **Step 5: Commit.** Message: "Run the scene-target path without an upscaler when screen-space effects need it".

### Task 11: View reconstruction and dominant light selection

**Files:**
- Create: `renderer/view_reconstruct.h` (header-only; mirrored in `renderer/shaders/view_space.slang`)
- Modify: `renderer/scene_lights.h/.cpp` (collector and selection)
- Modify: `renderer/vulkan_renderer.h/.cpp` (collect lit scene draws' lights while recording scene draws; selection at the boundary)
- Test: `tests/test_view_reconstruct.cpp`, `tests/test_scene_lights.cpp`

**Interfaces:**
- Consumes: `classify_light`, `channel_lighting` (Task 1).
- Produces:
  - `struct uc::ViewParams { float x_scale, y_scale, x_offset, y_offset, depth_a, depth_b; };`
  - `uc::ViewParams uc::view_params(const gx::Projection& scene_projection);`. It reconstructs `view = { (ndc.x - x_offset) * -z / x_scale, (ndc.y - y_offset) * -z / y_scale, z }` with `z = depth_b / (depth - depth_a)`. It holds for the perspective rows `gx::Projection` carries and uses Vulkan depth 0..1.
  - `struct gx::CollectedLight { LightKind kind; std::array<float,3> position, direction, color; uint32_t draws; };`
  - `void gx::SceneLightCollector::add(const DrawUniform&)`: dedupes lights equal within 1e-3 and counts the draws each light lights. Plus `clear()` and `const std::vector<CollectedLight>& lights() const`.
  - `std::optional<gx::CollectedLight> gx::select_dominant(std::span<const CollectedLight>, const std::optional<CollectedLight>& previous);`. Only directional and spot lights qualify. Score = `draws * max(color)`. The previous light is kept unless a new one scores ≥ 1.25× its current score.

- [ ] **Step 1: Write the failing tests.**
  - `test_view_reconstruct`: build a GX perspective projection (fovy 60°, aspect 16/9, near 1, far 1000) as the game's `gx::Projection` rows. Project view points `{0,0,-10}`, `{3,-2,-50}` and `{-1,1,-999}` to NDC and depth with the rows, reconstruct them with `view_params`, and check the error is < 1e-3 relative.
  - `test_scene_lights`:
    - two draws sharing one spot light give one collected light with `draws==2`;
    - `select_dominant` of an empty span is `nullopt`;
    - a point-only list gives `nullopt`;
    - with a previous light scoring 10 and a new one scoring 12, the previous is kept; with the new one at 13, the new one wins.
- [ ] **Step 2: Run them.** Command: `ctest --test-dir build -R "test_view_reconstruct|test_scene_lights" --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** The collector is filled in `render()`'s per-draw loop for draws `< scene_draws` (render thread only, no lock). It is cleared at `frame_start_`. `select_dominant` runs in `record_upscale`, and its result is stored in `VulkanRenderer::dominant_light_`.
- [ ] **Step 4: Run them.** Same command. Expected: PASS.
- [ ] **Step 5: Commit.** Message: "Add view-space reconstruction and dominant scene light selection".

### Task 12: GTAO

**Files:**
- Create: `renderer/occlusion_pass.h`, `renderer/occlusion_pass.cpp` (pattern: `RcasPass`)
- Create: `renderer/shaders/gtao.slang` (entries `gtaoMain`, `denoiseMain`), `renderer/shaders/occlusion_composite.slang` (entry `compositeMain`)
- Modify:
  - `renderer/CMakeLists.txt` (shaders, source);
  - `renderer/graphics_effects.h` (`enum class EffectLevel { off, low, medium, high, ultra }`, `EffectLevel ambient_occlusion=EffectLevel::off`, `needs_scene_targets` and `effects_need_gbuffer` true when it is not off);
  - `renderer/vulkan_renderer.h/.cpp` (owns `OcclusionPass`; `record_upscale` calls it before `upscaler_->evaluate` when the frame had a scene; `GpuPass::occlusion` around it);
  - options, config, CLI `--ambient-occlusion off|low|medium|high|ultra`, menu, launcher (combo, disabled without `features.effects`);
  - translations: `ambient_occlusion` ("Ambient occlusion (GTAO)" / "Затенение окружающего света (GTAO)"), `low`/`medium`/`high`/`ultra` ("Low"/"Низкое", "Medium"/"Среднее", "High"/"Высокое", "Ultra"/"Ультра").
- Test: `tests/renderer_upscale_smoke.cpp`, `tests/test_options.cpp`, `tests/test_options_config.cpp`, `tests/test_live_settings.cpp`

**Interfaces:**
- Consumes: `view_params` (Task 11), G-buffer and `decode` (Task 9), scene path (Task 10), `PassTimer` (Task 6).
- Produces:
  - `struct uc::OcclusionInputs { VkCommandBuffer command; UpscaleImage color, depth, gbuffer; VkExtent2D render_size; ViewParams view; EffectLevel ao; bool contact; std::optional<gx::CollectedLight> light; uint64_t frame; bool temporal_noise; };`. All images are in GENERAL on entry and on exit.
  - `class uc::OcclusionPass { void initialize(VkDevice, const VkPhysicalDeviceMemoryProperties&, const std::filesystem::path& shaders); void record(const OcclusionInputs&); void shutdown(); };`
  - Presets (XeGTAO slice/step counts and resolution):
    - low: 1 slice × 2 steps, half resolution;
    - medium: 2 × 2, half;
    - high: 3 × 3, full;
    - ultra: 9 × 3, full.

    Radius 0.5 view units and falloff 0.6 are the starting constants; tune them on the survey frames within the budget. Half-resolution AO is upsampled bilaterally by depth in the composite.
  - Composite: `color.rgb *= lerp(1, ao, (1 - fog))` for texels with `kGBufferHasNormal`. Other texels, and texels at the clear depth, are unchanged.
  - `temporal_noise`: true only when the upscaler is DLSS or FSR. Otherwise the noise index is 0 every frame.

- [ ] **Step 1: Write the failing tests.**
  - Options, config and live settings round trip for `ambient_occlusion`. The default is off. An invalid name gives the error `"invalid value for --ambient-occlusion"`.
  - `renderer_upscale_smoke --gtao`, crease fixture: a floor and a wall meeting at 90°, both lit and opaque, with an empty background. Render with AO off and with each preset.
    - Pixels within 5% of screen height of the crease are ≥ 10 levels darker than off.
    - Floor pixels far from the wall (> 40% screen away) change by ≤ 2 levels.
    - The background is unchanged.
    - A fogged copy of the fixture (fog factor 1 everywhere) is unchanged.
    - A frame without a scene (menu fixture: one 2D quad, no perspective draw) is identical to off, and `pass_ms(GpuPass::occlusion)==0`.
    - Toggling AO between off and medium 60 times keeps `descriptor_pool_creations()` and `texture_creations()` stable after the first cycle.
- [ ] **Step 2: Run them.** Command: `ctest --test-dir build -R "test_options|test_live_settings" --output-on-failure` and `build\renderer_upscale_smoke.exe --gtao --validation`. Expected: FAIL.
- [ ] **Step 3: Implement.**
  - `gtaoMain`: per texel, reconstruct the view position (`view_space.slang`), take the normal from the G-buffer, and run the XeGTAO horizon search over the preset's slices and steps. Write R8 AO.
  - `denoiseMain`: a 3×3 depth-aware blur.
  - `compositeMain`: upsample when half resolution, then multiply into `color` with the fog fade.
  - Reuse the scene targets' `STORAGE` usage. Create the AO images at the preset resolution and recreate them on size or preset change.
- [ ] **Step 4: Run them.** Same commands. Expected: PASS, no validation errors.
- [ ] **Step 5: Check the budget and look live.**
  - Run `python -m uctest run fight --game-arg=--ambient-occlusion --game-arg=<preset> --game-arg=--profile` once at 1080p with low and once with high.
  - Expected `occlusion` times on the RTX 4080: low ≤ 0.2 ms at 1080p, high ≤ 1.0 ms at 1440p (render scale 2 gives 1440p). If over budget, lower the step count of that preset and re-run once.
  - Capture frames with AO off and on at the same mark for the owner.
- [ ] **Step 6: Bench the off path.** One run, AO off in both builds, same 1% rule.
- [ ] **Step 7: Commit.** Message: "Add GTAO ambient occlusion with low to ultra presets".

### Task 13: Contact shadows

**Files:**
- Create: `renderer/shaders/contact_shadows.slang` (entry `contactMain`)
- Modify:
  - `renderer/occlusion_pass.cpp` (dispatch `contactMain` when `inputs.contact && inputs.light`; its R8 result is multiplied in the same composite);
  - `renderer/graphics_effects.h` (`bool contact_shadows=false`; included in `needs_scene_targets` and `effects_need_gbuffer`);
  - options, config, CLI `--contact-shadows`, menu, launcher (checkbox, disabled without `features.effects`);
  - translations: `contact_shadows` ("Contact shadows" / "Контактные тени").
- Test: `tests/renderer_upscale_smoke.cpp`, `tests/test_options.cpp`, `tests/test_options_config.cpp`

**Interfaces:**
- Consumes: `OcclusionPass`, `OcclusionInputs::light` (Task 12), `select_dominant` (Task 11).
- Produces: contact shadows at half resolution, 16 steps, maximum ray length 3% of the pixel's view depth, depth thickness 0.5% of view depth. The march goes toward the light: the constant direction for a directional light, toward the position for a spot light. The result is multiplied with strength 0.6, faded by fog, on `HasNormal` texels.

- [ ] **Step 1: Write the failing tests.**
  - Options and config round trip. The default is off.
  - `renderer_upscale_smoke --contact`, fixture: a box standing on a floor, lit by a directional light from the upper left (a GX light at position `{-1e6,1e6,1e6}`, lighting channel on, on every draw).
    - Floor pixels just right of the box base (within 2% of screen width) are ≥ 15 levels darker with contact shadows on.
    - Floor pixels far from the box are unchanged (≤ 1 level).
    - The same fixture with only a point light (no qualifying light) is identical to off.
    - With AO also on, both darken: crease pixels ≤ AO-only.
- [ ] **Step 2: Run them.** Command: `build\renderer_upscale_smoke.exe --contact --validation` and the option tests. Expected: FAIL.
- [ ] **Step 3: Implement** the contact pass and the option.
- [ ] **Step 4: Run them.** Same commands. Expected: PASS.
- [ ] **Step 5: Check the budget, determinism and the image live.**
  - Run `python -m uctest run fight --game-arg=--contact-shadows --game-arg=--ambient-occlusion --game-arg=medium --game-arg=--profile`. Expected: `occlusion` ≤ 0.4 ms at 1080p on the RTX 4080 for medium plus contact shadows.
  - Run `python -m uctest demo check fight2 --game-arg=--ambient-occlusion --game-arg=high --game-arg=--contact-shadows`. Expected: replay == recording (the guest is unaffected).
  - Capture frames off and on for the owner.
- [ ] **Step 6: Bench the phase.** One run, everything off in both builds: `dist/uc_before.exe` (before Task 1) versus the phase 3 exe, `--runs 3`. Expected: within 1%.
- [ ] **Step 7: Commit.** Message: "Add screen-space contact shadows toward the dominant scene light".

---

## After phase 3

Write the plan for phases 4–6 (shadow maps + PCSS, volumetric lighting, motion blur, tessellation) from `docs/spikes/S6-graphics-survey.md` and the spec, as `docs/superpowers/plans/<date>-graphics-enhancements-p4-6.md`.
