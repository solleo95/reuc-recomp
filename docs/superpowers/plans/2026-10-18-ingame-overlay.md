# In-game Settings Overlay, Pause, Inverted Bloom — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Insert opens an ImGui settings menu over the running game (graphics + controls, applied live and saved at once); P pauses the game by stepping it by 0; the bloom checkbox becomes "Disable glow around light sources (bloom)".

**Architecture:** The renderer owns a second ImGui context (`MenuOverlay`, same pattern as `PromptOverlay`) with the SDL3 platform and Vulkan renderer backends and draws it next to the prompts at present. The game side (`game/settings_menu.*`) builds the widgets through a callback, keeps an `Options` + `BindingEditor`, and applies every change through `LiveSettings`, which talks to the renderer, the frame limiter, game pacing and input, then saves `config.json`. Pause is a flag read by `hle_GameFrameWait` that writes a zero step.

**Tech Stack:** C++20, SDL3, Vulkan 1.3, ImGui 1.92.9b (`uc_imgui`, `uc_imgui_vulkan`), nlohmann json, CTest (`tests/check.h`), Python harness `scripts/uctest`.

**Spec:** `docs/superpowers/specs/2026-10-18-ingame-overlay-design.md`

**Execution status (2026-10-18):** Tasks 1–7 implemented and reviewed in 5710538..9cdc6af; Task 8 completed with the verification limits below. Original task checklists below describe the proposed sequence; actual API choices and deviations are recorded in `.superpowers/sdd/2026-10-18-ingame-overlay/progress.md`. Final verification resumed after Claude reached its session limit. Follow-up changes remain uncommitted; `dist` was staged without installing to the game directory.

## Global Constraints

- Hotkeys: Insert toggles the menu, P toggles pause; both are host keys and never reach the game; P does nothing while the menu is open.
- Main tab, in this order: render scale; upscaling Off/DLSS/FSR; quality DLAA (FSR: "Native AA")/Quality/Balanced/Performance/Ultra Performance; FPS limit (30, 60, 90, 120, 144, 165, 240, 360, unlimited — the launcher list); frame generation Off / DLSS X2 / FSR X2; "Other": "Disable glow around light sources (bloom)".
- Controls tab: prompt icon style; keyboard/mouse bindings (launcher editor behaviour: replace, add, clear, defaults).
- Every change applies live and is saved to `config.json` at once; anything that cannot apply in-process is greyed out with a tooltip ("changed in the launcher").
- Config key `graphics.bloom` keeps its meaning (true = glow); only the UI inverts.
- Labels come from the launcher table `launcher/translations.inc` (13 languages), language from the config.
- Pause = game step 0, audio output stream paused, input blocked; the game keeps rendering.
- The closed menu costs nothing measurable: demo bench (`python -m uctest bench <A> <B> --demo fight2`) within noise.
- Live tests only through the harness (AGENTS.md), never system input; one or two runs per question.
- Source edits only with Edit/Write (no heredoc/sed patches).

## Review Focus

1. Upscaler switched while frame generation is on, or FG switched while the upscaler is Off → the menu changes both consistently (FG needs a matching upscaler), no crash, saved config valid for the launcher.
2. A setting changed while paused → applies, game stays paused, menu stays usable; resume continues normally.
3. Renderer restart while guest packets are in flight → no lost completions (guest never hangs), no Vulkan validation errors.
4. Binding capture in the menu receives Insert/P/Escape → Insert closes nothing while capturing; the captured key is the one bound (same as launcher capture rules), P does not pause.
5. Window resize / alt-tab / focus loss while the menu is open or paused → menu state kept, relative mouse restored only when both are closed.

Each line has its test in the owning task (Tasks 2, 5, 6, 7).

---

## File Structure

| File | Responsibility |
|---|---|
| `game/game_pacing.{h,cpp}` | `pacing_for_limit`, live pacing mode, pause flag (replaces the spike) |
| `game/audio_output.{h,cpp}` | `pause(bool)` |
| `renderer/menu_overlay.{h,cpp}` | ImGui context for the menu, SDL3 + Vulkan backends, record into a command buffer |
| `renderer/vulkan_renderer.{h,cpp}` | `set_menu`, `menu_event`, `reconfigure_upscaling` (mode change + in-place restart) |
| `game/live_settings.{h,cpp}` | apply an `Options`/`Bindings` change to all subsystems, save config |
| `game/settings_menu.{h,cpp}` | widgets of both tabs, BindingEditor capture routing |
| `game/prompt_hooks.cpp` | thread-safe live binding update |
| `game/test_server.{h,cpp}`, `game/test_protocol.*` | `host_key` and `options` harness commands |
| `game/uc_main.cpp` | key routing, menu/pause state, harness hooks |
| `launcher/graphite_style.cpp` (split from `graphite.cpp`) + `launcher/ui_fonts.cpp` | shared style and font loading (`uc_ui_style` library) |
| `launcher/main.cpp`, `launcher/translations.inc` | inverted bloom checkbox, new strings |
| `scripts/uctest/scenarios.py`, `client.py` | `pause`, `overlay` scenarios; `host_key`, `set_options` client calls |

---

### Task 1: Spike — renderer restart in process

Throwaway proof that `shutdown()` + `configure_upscaling()` + `initialize()` works on the same window with Streamline (DLSS) and FSR, while the guest runs. Decides which upscaler/FG entries Task 5 greys out.

**Files:** temporary edits in `game/uc_main.cpp` only (env `UC_SPIKE_RESTART=<seconds>`: every N seconds cycle off → dlss → fsr → dlss+FG → fsr+FG).

- [ ] **Step 1:** In the main loop, at the cycle point: `renderer.poll_completions()` until `wait_for_pending` returns false, report completions to `packets`, `renderer.shutdown()`, `renderer.configure_upscaling(...)`, `renderer.initialize(window, ...)`, `set_prompt_style`, `set_bloom`, `warm_pipelines` (silent). Log time per restart.
- [ ] **Step 2:** Run `python -m uctest run perf --measure 40` with `UC_SPIKE_RESTART=6` and `--game-arg=--validation`. Expected: PASS, no validation errors, each restart logged under ~2 s, captures after each restart show the scene (look at them).
- [ ] **Step 3:** Record the outcome in the spec ("Owner's answers" section): which kinds restart cleanly. A kind that fails is greyed out in Task 5. Revert the spike edits (`git checkout game/uc_main.cpp`); keep the timing numbers.

### Task 2: Renderer — `reconfigure_upscaling` and menu overlay

**Files:**
- Create: `renderer/menu_overlay.h`, `renderer/menu_overlay.cpp`
- Modify: `renderer/vulkan_renderer.h`, `renderer/vulkan_renderer.cpp`, `renderer/CMakeLists.txt` (add `menu_overlay.cpp` to `uc_vulkan`)
- Test: `tests/renderer_smoke.cpp`

**Interfaces:**
- Produces:
  - `class MenuOverlay { void initialize(SDL_Window*, VkInstance, VkPhysicalDevice, VkDevice, VkQueue, uint32_t family, uint32_t slots); bool event(const SDL_Event&); void record(VkCommandBuffer, VkExtent2D, const std::function<void()>& build); void shutdown(); ImGuiContext* context() const; }` — own ImGui context, `ImGui_ImplSDL3_InitForVulkan` + `ImGui_ImplVulkan_Init` (dynamic rendering, RGBA8 like `PromptOverlay`); fonts from `uc::ui::load_fonts` (Task 4) — until Task 4 lands, Segoe UI only.
  - `void VulkanRenderer::set_menu(std::function<void()> build)` — empty = menu closed (no ImGui work at all); non-empty = `MenuOverlay::record` right after `prompt_overlay_.record` at `vulkan_renderer.cpp:1536`.
  - `bool VulkanRenderer::menu_event(const SDL_Event&)` — forwards to the overlay; true when ImGui wants the event.
  - `void VulkanRenderer::reconfigure_upscaling(UpscalerKind, UpscaleMode, bool sharpening, double sharpness, bool frame_generation, uint32_t multiplier, const std::function<void(uint64_t)>& completed)` — same kind/FG: store mode, drop `upscaler_` + scene targets, `resized_=true`; otherwise drain in-flight work reporting each completed serial through `completed`, `shutdown()`, re-`initialize()` with the saved window/validation/shaders/scale/aspect, restore prompt style, bloom and menu, warm pipelines.
  - `UpscalerKind VulkanRenderer::active_upscaler() const; bool VulkanRenderer::frame_generation_active() const` — what the device really runs after fallbacks (the menu shows this).
- Remove the "fixed for the process" comment on `configure_upscaling`.

- [ ] **Step 1: Failing smoke test** in `tests/renderer_smoke.cpp`: render a few packets with upscaler off, call `reconfigure_upscaling(fsr, quality, ...)`, render more packets; `CHECK(renderer.active_upscaler()==uc::UpscalerKind::fsr)`; every submitted serial is reported exactly once across the switch (collect from `poll_completions()` and the callback, compare with submitted serials); then `reconfigure_upscaling(fsr, performance, ...)` → `CHECK(renderer.upscale_internal_extent().width < previous_width)`; then back to off → `CHECK(renderer.active_upscaler()==uc::UpscalerKind::off)`; `CHECK(renderer.validation_errors()==0)`.
- [ ] **Step 2:** `ctest --test-dir build -R renderer_smoke --output-on-failure` → FAIL (no member `reconfigure_upscaling`).
- [ ] **Step 3:** Implement `MenuOverlay` and the renderer members above.
- [ ] **Step 4:** Smoke test for the menu: `set_menu([]{ ImGui::Begin("t"); ImGui::Text("menu"); ImGui::End(); })`, render a presenting packet, capture; `CHECK` the captured frame differs from the same packet without menu; `set_menu({})` → identical to the plain frame.
- [ ] **Step 5:** `ctest --test-dir build -R renderer_smoke --output-on-failure` → PASS.
- [ ] **Step 6:** Commit `renderer: live upscaler reconfiguration and menu overlay`.

### Task 3: Pause by zero step, live pacing, audio pause

**Files:**
- Modify: `game/game_pacing.h`, `game/game_pacing.cpp` (remove the `UC_SPIKE_ZERO_STEP` block and the `<windows.h>` include), `game/uc_main.cpp:219-226`, `game/audio_output.h`, `game/audio_output.cpp`, `scripts/uctest/scenarios.py` (replace `spike_zero_step`)
- Test: `tests/test_game_pacing.cpp` (new, register in `tests/CMakeLists.txt` like `test_options_config`)

**Interfaces:**
- Produces:
  - `struct PacingMode { bool unlock_60; uint32_t game_hz; };`
  - `PacingMode pacing_for_limit(std::optional<uint32_t> fps_limit, uint32_t game_hz_override, bool unlock_60fps)` — exactly the rule of `uc_main.cpp:219-225`.
  - `void game_pacing_set_mode(PacingMode)` — atomics, picked up at the next `hle_GameFrameWait` (reset `deadline_ticks`/`last_boundary_ticks` when the mode changes).
  - `void game_pacing_set_paused(bool); bool game_pacing_paused();` — while paused `hle_GameFrameWait` writes step 0 to `object+4`/`object+8` of `kClock` after `0x80158208` and sets `step_scale=0` (the spike's placement).
  - `void AudioOutput::pause(bool)` — `SDL_PauseAudioStreamDevice` / `SDL_ResumeAudioStreamDevice`.

- [ ] **Step 1: Failing tests** `test_game_pacing.cpp`: `pacing_for_limit(std::nullopt,0,false)` → `{false,0}`; `(30,0,false)` → `{false,0}`; `(60,0,false)` → `{true,0}`; `(144,0,false)` → `{false,144}`; `(0,0,false)` → `{false,480}`; `(1000,0,false)` → `{false,480}`; `(144,240,false)` → `{false,240}`; `(std::nullopt,0,true)` → `{true,0}`.
- [ ] **Step 2:** `ctest --test-dir build -R test_game_pacing --output-on-failure` → FAIL.
- [ ] **Step 3:** Implement; `uc_main.cpp` calls `game_pacing_init(mode.unlock_60, mode.game_hz)` from `pacing_for_limit`.
- [ ] **Step 4:** ctest → PASS.
- [ ] **Step 5 (run after Task 7, which adds `host_key`):** Harness scenario `pause` (shape of the spike): gameplay, cutscene start of chapter 1 (the mission intro), and a QTE if reachable by the bot: captures at +10/+100/+300 ticks of pause pixel-identical, enemy roots/HP and `view` unchanged, `retraces` still advancing; after resume the scene moves. Run in default (unlimited) and with `--game-arg=--fps-limit --game-arg=60` and `30`.
- [ ] **Step 6:** Commit `game: pause by zero step, live pacing mode, audio pause`.

### Task 4: Shared UI style, fonts and labels; inverted bloom in the launcher

**Files:**
- Create: `launcher/graphite_style.cpp` (move `graphite_style()` out of `graphite.cpp`), `launcher/ui_fonts.h`, `launcher/ui_fonts.cpp`
- Modify: `launcher/CMakeLists.txt` (new `uc_ui_style` STATIC: `graphite_style.cpp ui_fonts.cpp`, links `uc_imgui`; `uc_launcher` links it), `launcher/main.cpp` (font loading → `load_fonts`; bloom toggle), `launcher/translations.inc`, `launcher/localization.cpp` (key list if needed)
- Test: `tests/test_localization.cpp` (new or existing test of `uc_launcher_locale`), launcher smoke (`uc_launcher --smoke`)

**Interfaces:**
- Produces: `namespace uc::ui { struct Fonts { std::vector<std::vector<char>> bytes; ImFont* sans=nullptr; ImFont* serif=nullptr; }; Fonts load_fonts(ImFontAtlas&); }` — the launcher's current font set (Segoe UI 16 + msyh/malgun/msgothic merged; Georgia 22 + merged fallbacks); throws on the same conditions.
- New translation keys (all 13 columns): `bloom` text replaced by "Disable glow around light sources (bloom)" / "Выключить свечение вокруг источников света (bloom)" / …; `menu_main` ("Main" / "Основное"), `menu_controls` ("Controls" / "Управление"), `other` ("Other" / "Прочее"), `framegen_dlss` ("DLSS X2"), `framegen_fsr` ("FSR X2"), `launcher_only` ("Changed in the launcher" / "Меняется в лаунчере"), `paused` is not needed (no pause text).
- Launcher bloom: `bool disable_bloom=!options.bloom; toggle("##bloom",tr("bloom"),disable_bloom); options.bloom=!disable_bloom;`

- [ ] **Step 1: Failing test:** `CHECK(uc::launcher::text("ru","bloom")=="Выключить свечение вокруг источников света (bloom)")`, `CHECK(uc::launcher::text("en","bloom")=="Disable glow around light sources (bloom)")`, and every new key is non-empty in every language of `languages()`.
- [ ] **Step 2:** ctest → FAIL.
- [ ] **Step 3:** Translations, style/font split, launcher toggle.
- [ ] **Step 4:** ctest → PASS; `build\…\uc_launcher.exe --smoke <scratch dir>` → "launcher smoke passed" (it checks glyph coverage of every key in every language).
- [ ] **Step 5:** Commit `launcher: invert bloom option, share UI style and fonts`.

### Task 5: LiveSettings — apply and save

**Files:**
- Create: `game/live_settings.h`, `game/live_settings.cpp` (in `uc_game` or a new `uc_live_settings` lib linked by `uc`)
- Modify: `game/prompt_hooks.cpp` / `prompt_hooks.h` (live bindings), `game/frame_limit.h` (`set_limit`)
- Test: `tests/test_live_settings.cpp` (new)

**Interfaces:**
- Consumes: Task 2 renderer members, Task 3 `pacing_for_limit`/`game_pacing_set_mode`.
- Produces:
  - `struct SettingsTargets { std::function<void(double)> render_scale; std::function<void(UpscalerKind,UpscaleMode,bool,double,bool,uint32_t)> upscaling; std::function<void(std::optional<uint32_t>)> fps_limit; std::function<void(bool)> bloom; std::function<void(PromptStyle)> prompt_style; std::function<void(const Bindings&)> bindings; };` — `uc_main` wires them to renderer/frame limit/pacing/input/prompt hooks; tests use recorders.
  - `class LiveSettings { LiveSettings(std::filesystem::path config, ConfigDocument loaded, Options current, SettingsTargets); const Options& options() const; const Bindings& bindings() const; const std::optional<std::string>& error() const; void apply(const Options& next); void apply(const Bindings& next); };` — calls only the targets whose fields changed (upscaling: when any of kind/mode/sharpening/sharpness/FG/multiplier changed), then `store_options` + `save_config`; on success the saved document becomes the baseline; on failure `error()` is set and the in-memory change stays applied; a refused identical change is not re-saved.
  - `std::optional<uint32_t> FrameLimit::set_limit(std::optional<uint32_t>)` — same interval rule as the constructor.
  - `void prompt_hooks_set_bindings(const Bindings&)` — stores under a mutex; the guest side swaps `assignments` at the next `PromptScope` construction.
  - Frame generation coupling rule (pure, tested): `Options with_frame_generation(Options, FrameGenChoice)` with `enum class FrameGenChoice { off, dlss, fsr }` — dlss/fsr set `frame_generation=true`, `frame_generation_multiplier=2` unless the current upscaler already matches (then keep the saved multiplier), and switch `upscaler` to the matching kind; off sets `frame_generation=false`. Choosing upscaler Off also sets `frame_generation=false`.

- [ ] **Step 1: Failing tests** (`test_live_settings.cpp`, temp config file in `%TEMP%`):
  - changing only `bloom` calls only the bloom target and the saved file has `graphics.bloom==false`;
  - changing `upscale_mode` calls `upscaling` once; changing nothing calls nothing and does not touch the file (mtime unchanged);
  - `fps_limit` 144 → target called with 144, file `graphics.fps_limit==144`; unlimited (0) → `options_from_config(load_config(path)).fps_limit==0u`;
  - bindings change → `bindings` target called, file `input` equals the new bindings;
  - config file replaced on disk by another writer → `apply` sets `error()` containing "changed since loading", target still called;
  - `with_frame_generation(options_with(fsr), FrameGenChoice::dlss)` → `upscaler==dlss && frame_generation && multiplier==2`; DLSS ×3 saved + choose dlss → multiplier stays 3; upscaler Off chosen while FG on → `frame_generation==false`.
- [ ] **Step 2:** ctest → FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** ctest → PASS.
- [ ] **Step 5:** Commit `game: live settings application and saving`.

### Task 6: Settings menu and key routing

**Files:**
- Create: `game/settings_menu.h`, `game/settings_menu.cpp`
- Modify: `game/uc_main.cpp` (event loop at `:665-677`, `service_frame`, startup wiring after `renderer.initialize`), `game/CMakeLists.txt` (link `uc_ui_style`, `uc_launcher_locale`, `uc_binding_editor`)

**Interfaces:**
- Consumes: Task 2 `set_menu`/`menu_event`/`active_upscaler`/`frame_generation_active`, Task 3 pause/audio, Task 4 fonts/labels, Task 5 `LiveSettings`, `probe_upscalers` (`renderer/upscaler_probe.h`).
- Produces:
  - `class SettingsMenu { SettingsMenu(LiveSettings&, std::string language, std::function<FeatureSupport()> features); void build(); bool capturing() const; void capture_event(const SDL_Event&); };` — `build()` is the `set_menu` callback: two tabs per Global Constraints, launcher look (`graphite_style`, sizes scaled to the output height), unsupported or restart-failed entries disabled with the probe reason or `launcher_only` as tooltip; the error line of `LiveSettings::error()`.
  - `struct HostKeys { bool menu_open=false, paused=false; }` and `enum class HostKeyResult { none, menu_toggled, pause_toggled }` with `HostKeyResult host_key(HostKeys&, SDL_Scancode, bool menu_capturing)` (pure; tested): Insert toggles the menu unless capturing; P toggles pause unless the menu is open; other keys → none.
- Main loop rules: key-down of Insert/P (no repeat) go through `host_key` and are not passed on; menu open → events to `settings_menu.capture_event` when capturing else `renderer.menu_event`, nothing to `HostInput`; menu open or paused → `SDL_SetWindowRelativeMouseMode(false)` only for the menu (cursor needed), `input.focus(false)`; both closed → relative mode on (non-controlled runs) and `input.focus(true)` once; paused → `game_pacing_set_paused(true)`, `audio->pause(true)`.
- Feature probe: `std::async(std::launch::async, probe_upscalers, exe_dir())` started after `renderer.initialize`; the menu shows "Checking this PC…" until ready.

- [ ] **Step 1: Failing tests** (`tests/test_host_keys.cpp`): Insert → menu open; Insert again → closed; P with menu closed → paused; P with menu open → no change; Insert while capturing → no change and result `none`; key repeat is filtered by the caller (not tested here).
- [ ] **Step 2:** ctest → FAIL. **Step 3:** implement `host_key`, `SettingsMenu`, main loop wiring. **Step 4:** ctest → PASS.
- [ ] **Step 5:** Commit `game: in-game settings menu on Insert, pause on P`.

### Task 7: Harness commands and scenarios

**Files:**
- Modify: `game/test_server.h/.cpp` and the request parser (`parse_test_request`, `TestRequestKind`), `game/uc_main.cpp` (`service_frame` handles `Pump::host_keys` and `Pump::options` with the same code paths as SDL keys and the menu), `scripts/uctest/client.py`, `scripts/uctest/scenarios.py`, `docs/testing/test-server.md`
- Test: existing protocol test of the request parser (`tests/test_test_protocol.cpp` or where `parse_test_request` is tested)

**Interfaces:**
- Produces: request `{"cmd":"host_key","code":<SDL scancode>}` → `Pump::host_keys` (vector) → `host_key(...)`; request `{"cmd":"options","graphics":{...},"input":...}` → merged into a copy of the current config document, `options_from_config`, `LiveSettings::apply` (same path as the menu). Client: `Game.host_key(code)`, `Game.set_options(graphics: dict)`. Frame JSON gains `"menu_open"` and `"paused"`.
- Scenarios: `pause` (Task 3 Step 5 checks), `overlay` (open menu via Insert, capture → menu visible; while open, `g.press` fire is not consumed by the game; `set_options` render scale 1 → 2, quality, bloom off, prompt style, FPS limit 60 → unlimited, upscaler off → fsr → dlss → off with FG on/off — after each: capture, `report.json` lists restart time and that the game advanced; config file in the run dir reflects each value; Escape/Insert closes; P during menu ignored; pause + Insert opens menu over the frozen picture; window resize while open keeps the menu).

- [ ] **Step 1: Failing parser tests** for both commands (valid, missing fields → error reply).
- [ ] **Step 2:** ctest → FAIL. **Step 3:** implement. **Step 4:** ctest → PASS.
- [ ] **Step 5:** `python -m uctest run pause --speed 8`, then `python -m uctest run overlay` → PASS; look at every capture yourself.
- [ ] **Step 6:** Commit `harness: host keys, live options, pause and overlay scenarios`.

### Task 8: Verification

- [x] **Step 1:** `build.bat --no-install` and `ctest --test-dir build --output-on-failure` → all pass (114/114).
- [x] **Step 2:** Closed-menu cost: baseline 5710538 staged as `dist/uc_before_overlay.exe`; `py -3.13 -m uctest bench dist/uc_before_overlay.exe dist/uc.exe --demo fight2 --runs 3` → +0.8% mean wall time, 95% CI [-9.2%, +10.8%], INCONCLUSIVE. All six runs performed 1,328,859 draws; best vs best +1.2%. No significant difference detected; machine drift leaves this comparison unable to exclude a small regression.
- [x] **Step 3:** `py -3.13 -m uctest run fight --measure 15` → PASS at speed 1: active mean 149.9 FPS, p5 140 FPS, frame-gap p99 9.2 ms (`build/autotest/1792353629092599300/report.json`). This confirms live operation; the deterministic benchmark above supplies the performance comparison.
- [x] **Step 4:** Updated spec status, `docs/testing/test-server.md`, and execution ledger with verification evidence and remaining limitations.

### Verification notes (2026-10-18)

- Restored four accidentally commented-out overlay checks: bloom on/off and sculpted/monochrome prompts. The expanded `overlay` run passed all 17 changes, saving each setting and preserving 1.5x render scale across renderer restarts (`build/autotest/1792353115167011400/report.json`). Menu captures were inspected, including bloom and DLSS X2.
- Corrected a false pause failure caused by camera float roundoff: matrix entries allow absolute error 1e-5, while actors, HP and player health remain exact. Added regression checks for roundoff, actual movement, damage, missing camera and NaN; observed RED then GREEN. Unlimited pause passed three consecutive runs; 30 FPS and 60 FPS each passed once, including title, gameplay, menu over pause and resume.
- Baseline was rebuilt from a clean 5710538 archive using local SDK sources, current generated code and the same Release/ThinLTO/PGO settings; no network fetch or main checkout change was needed. Scratch sources and the baseline executable remain under `build/overlay-baseline-*` for reproducibility.
- Cutscene/QTE pause, live mouse binding capture, menu resize/focus transitions and audio continuity were not verified in this final pass. Existing sub-pixel bone movement and advancement of the guest audio timeline during pause remain limitations; other deferred review findings remain in the execution ledger.
