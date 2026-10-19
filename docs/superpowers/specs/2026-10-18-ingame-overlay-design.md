# In-game settings overlay, pause key, inverted bloom option

Date: 2026-10-18. Status: implemented in 5710538..9cdc6af; final verification and remaining limitations recorded in the implementation plan and `docs/testing/test-server.md`.

## Intent

Change the settings a player touches most without quitting to the launcher, and freeze the game on demand.
Today every graphics/control option lives only in the launcher (`launcher/main.cpp`) and is read once by `uc.exe` at start.

## What the owner asked for (theirs)

- **Insert** opens/closes an ImGui overlay menu inside the game.
- The game keeps running under the open menu; input to the game is blocked while it is open.
- Tab **Main** (same choices as the launcher):
  1. Render scale
  2. Upscaling: Off / DLSS / FSR
  3. Upscaling quality: DLAA (Native AA for FSR) / Quality / Balanced / Performance / Ultra Performance
  4. FPS limit
  5. Frame generation: Off / DLSS X2 / FSR X2
  6. Section "Other": "Disable glow around light sources (bloom)"
  7. RCAS sharpening toggle and strength slider, enabled when upscaling is active (owner follow-up, 2026-10-18).
- Tab **Controls**:
  1. Prompt icon style
  2. Keyboard/mouse bindings
- Every setting takes effect in real time and is saved to `config.json` immediately on change.
- Bloom option semantics are inverted, in the launcher too: the checkbox is named "Disable glow around light sources (bloom)"; checked = no glow, unchecked = glow.
- **P** pauses / resumes the game: the game simply freezes, no menu, nothing drawn.
- Owner follow-up, 2026-10-18: narrow, draggable window with one control per row, both tabs visible, half-size checkboxes with vertically aligned labels. Theme scaling starts from fresh base metrics every frame so high-resolution windows cannot accumulate size changes.
- Owner refinement, 2026-10-19: reduce controls and spacing by 25%; keep the existing window dimensions, fonts and checkbox squares. Control widths use 75% of the available row; vertical padding accounts for the unchanged text size.

## Non-goals

- No other launcher options in the overlay (window mode, skip intro/tutorials, language, game folder, audio). *(builder's call from the owner's list; RCAS added by the owner follow-up)*
- No pause screen, no "Paused" text, no audio fade.
- No gamepad navigation of the overlay.

## Anti-goals

- A setting that silently needs a restart. If something cannot apply live, that is a defect, not a label.
- Hitches or FPS loss while the menu is closed. The closed overlay costs nothing measurable (demo bench, AGENTS.md method).
- Losing or corrupting `config.json` (the launcher must still read everything the game writes).

## Current state (facts from the code)

- ImGui 1.92.9b is already built (`launcher/CMakeLists.txt`: `uc_imgui` with the SDL3 platform backend, `uc_imgui_vulkan`) and linked into the renderer; `renderer/prompt_overlay.cpp` already draws with its own ImGui context through `ImGui_ImplVulkan` on the final image (`vulkan_renderer.cpp:1536`).
- Settings model: `uc::Options` (`game/options.h`), read/written by `options_from_config` / `store_options` (`game/options_config.cpp`); bindings live in `ConfigDocument::input`; launcher saves with `uc::save_config` (it refuses when the file changed since loading).
- Launcher translations: `launcher/localization.cpp` + `translations.inc`, 13 languages, chosen language stored in the config.
- Live-capable today: `set_render_scale` (rebuilds targets), `set_bloom`, `set_prompt_style`, `NativeInput::bindings` (mutex-guarded).
- Not live today:
  - `configure_upscaling` is only allowed before `initialize()`; the upscaler kind and frame generation decide device creation (Streamline before the instance for DLSS, extra queues + FSR swapchain for FSR FG, motion-output shader variant, device extensions).
  - FPS limit also picks the game update mode at start (`uc_main.cpp:219`, `game_pacing_init`): ≤30 original, 31..60 retrace 60 Hz, >60/unlimited timer-driven step.
  - `prompt_hooks_init` resets guest-owned state; it is not safe to call from the main thread while the guest runs.
- Game step: `hle_GameFrameWait` (`game/game_pacing.cpp`) owns the game clock's per-frame step and `step_scale`; retraces keep running during a step-0 pause, so the "no progress" watchdog (`uc_main.cpp:719`) is unaffected.

## Design

### Overlay

- New renderer-side overlay with its own ImGui context (same pattern as `PromptOverlay`), drawn on the final image after the prompts, so it sits above the game HUD and is not fed to the upscaler/frame generation as scene content.
- Look follows the launcher (graphite style, Segoe UI + CJK fallback fonts, same labels from the launcher translation table in the language saved in the config). *(builder's call)*
- Main loop: Insert toggles the menu. While open, SDL events go to ImGui (`ImGui_ImplSDL3_ProcessEvent`) instead of `HostInput`; relative mouse mode is off so the cursor is visible; `NativeInput` loses focus so held keys are released. Closing restores relative mode and focus. Insert and P are host keys and never reach the game. *(builder's call)*
- Supported DLSS/FSR/FG features come from the same probe the launcher uses (`probe_upscalers(exe_dir())`), run once in the background at start; unsupported entries are disabled with the reason as tooltip, as in the launcher.
- Frame generation entries only offer the generator of the active upscaler (DLSS X2 needs DLSS upscaling, FSR X2 needs FSR); picking a generator of the other kind switches the upscaler to match. *(builder's call)* A saved DLSS multiplier above 2 from the launcher is shown as is.

### Applying each setting live

| Setting | How it applies |
|---|---|
| Render scale | `set_render_scale` (existing rebuild) |
| Quality mode (same upscaler) | drop `upscaler_` and the scene targets; the next `rebuild()` recreates them |
| Upscaler kind, frame generation | **renderer restart in place**: finish in-flight GPU work and report its completions, `shutdown()`, `configure_upscaling(...)`, `initialize()` on the same window, pipelines from the on-disk cache. The guest keeps running; expect a short hitch (target under ~1-2 s, measured during implementation). Textures and meshes re-upload from their guest-side sources; EFB copy textures made before the restart are lost, so one or two frames may show missing post effects. |
| FPS limit | new `FrameLimit` interval at once; the pacing mode (original / 60 Hz / timer step) is handed to the guest through atomics read at the next `GameFrameWait` |
| Bloom | `set_bloom` |
| Prompt style | `set_prompt_style` |
| Bindings | `NativeInput::bindings`; prompt hook assignments handed to the guest side and picked up on its thread |

### Saving

On every change the game writes the option into its loaded `ConfigDocument` via `store_options` and calls `save_config` (same validation as the launcher). A refused save shows an error line in the menu and is not retried until the next change. Bindings use the launcher's `BindingEditor` (capture, add, clear, defaults).

### Bloom inversion

Config key `graphics.bloom` keeps its meaning (true = glow) so existing configs keep their look; only the UI inverts: the checkbox is bound to `!bloom` and relabelled in all 13 languages. *(builder's call)*

### Pause (P)

- P toggles a pause flag in the host; P inside the open menu does nothing. *(builder's call)*
- **Pause = game step 0** (owner's choice after a probe). The guest clock, VI retraces and the game loop keep running and the game keeps rendering every frame; in `hle_GameFrameWait` the game clock's step (`+4`, `+8` of the clock object) is written as 0 and `step_scale` is 0, so every system scaled by the step stops.
  - Probe 2026-10-18 (train car, 4 zombies, timer-step mode): frames at +10, +100 and +300 ticks of pause were pixel-identical (enemies, camera, HUD, the reticle's spinning ring); enemy positions/HP, camera matrix and player life unchanged; after resume the game moved on normally. The first few ticks after the flag still finish frames already queued.
  - Must also hold in the original 30 Hz and the 60 Hz retrace modes (the step is overwritten after the guest computed it in every mode).
- Audio: the SDL audio output stream is paused while paused (the guest's mixer keeps running; music may resume a little further on).
- Input: nothing reaches the game while paused (as with the open menu); held keys are released.
- Because the game keeps rendering, the menu over a paused game and settings changed during the pause need no extra path; they show at once.
- To check during implementation: cutscenes, movies, QTE, the game's own menus with step 0 (anything still moving is a defect to fix there, e.g. per-call systems).
- Ignored in deterministic/demo runs and harness-controlled runs unless the harness sends it explicitly.

### Testing

- Unit tests: config round-trip of every overlay option; pacing-mode switch; bloom inversion label/value mapping.
- Pause: harness scenario (the probe's shape) in gameplay, a cutscene and a QTE: captures during pause identical, state unchanged, game continues after resume.
- The overlay and pause must be drivable through the test harness (AGENTS.md: no system input): the harness can send Insert/P and a "set option" path, and capture frames.
- Performance: demo bench closed-menu build vs current build, no regression beyond noise.
- Live: owner-visible check of each setting switching in a fight scene.

## Builder's calls after review

- Threads: the renderer is owned by the SDL main thread (`owner()` checks); the overlay, settings application and the renderer restart all run there, no queues needed.
- Saving: every option is a drop-down or checkbox (no sliders), so each pick saves once. After its own save the game keeps the written document as its baseline (as the launcher does). The launcher closes once the game starts.
- Shared code: `uc_binding_editor` and `uc_launcher_locale` are already libraries; the graphite style and the upscaler probe move into small libraries linked by both targets.
- Translations: the builder writes the new strings for all 13 languages; existing strings stay untouched except the bloom label.
- Pacing switch: a short step glitch at the moment the FPS limit crosses a mode boundary is acceptable; no restriction of the list.
- Order of work: the renderer restart is spiked first; the rest does not depend on it.

## Owner's answers

- Renderer restart: the goal is that everything switches inside the process. Whatever cannot be made to work in-process is shown greyed out in the menu with a tooltip (changed in the launcher). No self-relaunch of the game.
- Menu while paused: Insert opens the menu over the frozen picture (free with the step-0 pause, which keeps rendering).
- Pause works by game step 0, audio stream paused, input blocked; cutscenes and QTE are checked during implementation.

- Renderer restart probe 2026-10-18 (perf scene, three full cycles off → DLSS → FSR → DLSS+FG → FSR+FG → off): every combination switches in-process, no crash, picture correct afterwards. Freeze: DLSS ~2.0-2.2 s (Streamline load), FSR/FSR+FG/off 0.6-0.8 s. Needed fix: the Streamline SDK helpers cache plugin functions in statics; after slShutdown the plugins unload, so calls now resolve through `slGetFeatureFunction` each time (`UC_SL_CALL`). The renderer is replaced as a whole object (fresh state); EFB copies of the old one are drawn blank until copied again.
- A short picture freeze during the renderer restart (upscaler kind / frame generation switch) is acceptable.

## Left to the builder

Overlay layout details, widget sizes, file/class names, how the harness exposes Insert/P, exact order of teardown in the renderer restart.
