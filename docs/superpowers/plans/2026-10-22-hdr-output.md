# HDR Output Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** HDR10 output with Peak / Paper White / UI brightness, launcher + in-game settings, automatic SDR fallback when Windows HDR is off.

**Architecture:** The game keeps rendering into the 8-bit EFB and the per-slot output image (captures stay SDR). In HDR the final slot→swapchain blit becomes a fullscreen encode pass: gamma-2.2 decode (true black), luminance-only highlight expansion from Paper White to Peak above a knee, BT.709→BT.2020, PQ into an `A2B10G10R10_UNORM_PACK32` / `HDR10_ST2084` swapchain. Port overlays (menu, prompts) draw into a separate RGBA8 layer composited at UI nits. Frame generation's HUD-less copy goes through the same encoder.

**Tech Stack:** C++20, Vulkan 1.3 dynamic rendering, Slang→SPIR-V, SDL3 3.2 (`SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN`, `SDL_EVENT_WINDOW_HDR_STATE_CHANGED`), Dear ImGui.

**Spec:** the design agreed in chat on 2026-10-22 (summarised in Goal/Architecture).

## Global Constraints

- Config keys in `graphics`: `hdr` (bool, default false), `hdr_peak_nits` 1..2000 (1000), `hdr_paper_white_nits` 80..500 (200), `hdr_ui_nits` 80..500 (200).
- HDR is active only when: setting on AND SDL reports window HDR AND the surface offers `A2B10G10R10_UNORM_PACK32` + `VK_COLOR_SPACE_HDR10_ST2084_EXT`. Otherwise SDR path unchanged.
- Native game HUD brightness is out of scope. No black-level slider, no HDR metadata.
- Sliders are disabled while the checkbox is off; changes apply live.

## Review Focus

- Windows HDR toggled while running → swapchain rebuilt in the new mode (event → `resized_`).
- Peak below Paper White → output clamps at Peak, no inversion (math test).
- Black input → PQ code 0 (math test).
- Frame generation on + HDR → hudless matches back-buffer format (FSR hudless format follows back buffer).
- Old config without HDR keys loads with defaults; out-of-range values are refused (config test).

---

### Task 1: HDR math + settings type
**Files:** Create `renderer/hdr_math.h`; Test `tests/test_hdr_math.cpp` (+ tests/CMakeLists.txt).
- `struct HdrSettings { bool enabled=false; uint32_t peak_nits=1000, paper_white_nits=200, ui_nits=200; bool operator==(const HdrSettings&) const=default; };`
- `float hdr_expand(float y, float paper, float peak)` → nits; `float pq_encode(float nits)`; `constexpr float kHdrKnee=0.6f`.
- Tests: `hdr_expand(0)=0`, `hdr_expand(k)=k*paper`, `hdr_expand(1)=peak`, monotonic, `peak<paper` → `min(y*paper,peak)`; `pq_encode(0)<1/1023`, `pq_encode(10000)=1`, `pq_encode(100)≈0.508`.

### Task 2: Options/config/live settings
**Files:** `game/options.h`, `game/options_config.cpp`, `game/live_settings.{h,cpp}`, `tests/test_options_config.cpp`.
- `Options::hdr` (HdrSettings); read/validate/write the four keys; `SettingsTargets::hdr(const HdrSettings&)`.

### Task 3: Renderer HDR path
**Files:** Create `renderer/hdr_pass.{h,cpp}`, `renderer/shaders/hdr.slang`; Modify `renderer/vulkan_renderer.{h,cpp}`, `renderer/fsr_frame_generation.cpp`, `renderer/CMakeLists.txt`, `game/uc_main.cpp`.
- `VulkanRenderer::set_hdr(const HdrSettings&)`, `hdr_state_changed()`, `configure_hdr()` before initialize.
- Instance ext `VK_EXT_swapchain_colorspace` when present; rebuild picks HDR format; swapchain image views; per-slot UI layer; encode pass at present and for hudless.

### Task 4: UI
**Files:** `launcher/main.cpp`, `game/settings_menu.cpp`, `launcher/translations.inc`.
- Checkbox + three sliders (disabled when off), hint that HDR follows Windows.

### Task 5: Verify
- `build.bat --no-install`, `ctest`, one harness run (`uctest run train_gameplay`) with HDR on in config.
