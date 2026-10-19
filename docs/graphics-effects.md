# Graphics effects

Design: `docs/superpowers/specs/2026-10-19-graphics-enhancements-design.md`; plan of phases 0–3: `docs/superpowers/plans/2026-10-19-graphics-enhancements-p0-3.md`; survey of the game's lighting: `docs/spikes/S6-graphics-survey.md`.

All effects live in the renderer (GPU passes recorded on the render thread); the guest threads are never touched and no effect reads pixels back. Every one is a separate option in the launcher (Graphics page) and in the in-game menu (Insert, Effects tab), saved in `config.json` and overridable on the command line. All off with anisotropy 1 is the original picture.

| Option | Config (`graphics.*`) | CLI | Values (default) | Needs |
|---|---|---|---|---|
| Anisotropic filtering | `anisotropy` | `--anisotropy` | 1, 2, 4, 8, 16 (16) | `samplerAnisotropy` (clamped to the GPU) |
| SMAA 1x | `upscaler` | `--upscaler smaa` | a value of Anti-aliasing / upscaling | the effects path |
| Per-pixel lighting | `per_pixel_lighting` | `--per-pixel-lighting` | on/off (off) | the effects path |
| Ambient occlusion (GTAO) | `ambient_occlusion` | `--ambient-occlusion` | off, low, medium, high, ultra (off) | the effects path |
| Screen-space shadows | `screen_space_shadows` | `--screen-space-shadows` | off, low (contact ray), medium, high, ultra (longer rays: 0.4 / 0.8 / 1.5 m) (off) | the effects path, a directional or spot light in the scene |
| Motion blur | `motion_blur` | `--motion-blur` | off, low, medium, high, ultra (off) | the effects path; skipped on camera cuts |

"The effects path" = `VK_EXT_dynamic_rendering_unused_attachments` + `independentBlend` (the same the upscalers need); without it those options are greyed out with a reason (`uc.exe --features` reports `effects`, `effects_reason`, `anisotropy_max`).

## How it renders

* The 3D scene of a frame renders into the internal scene targets (colour RGBA8, depth D32, motion RG16F, G-buffer RGBA16) up to the scene boundary (`gx::SceneBoundary`). Without an upscaler the targets exist only while an effect needs them (a blit pass-through at ratio 1); switching that on or off rebuilds the targets, every other toggle is live.
* G-buffer (`renderer/gbuffer.h`): octahedral view normal, GX fog factor, flags; written by depth-writing draws only.
* At the boundary, before the upscaler: `renderer/occlusion_pass.cpp` runs GTAO (horizon search at half or full resolution, depth-aware blur) and the contact-shadow march, then multiplies both into the scene colour, faded by the fog factor.
* Per-pixel lighting is a push-constant bit in the motion shader variants: the GX lighting equations run on the interpolated view normal and position instead of per vertex.
* SMAA is an `Upscaler` at ratio 1 (compute, `third_party/smaa`); no jitter, no vectors.
* Presets (budget per effect on the owner's RTX 4080, measured with the pass timestamps): GTAO medium + contact shadows 0.30 ms and high 0.50 ms at the harness size (limits 0.4 / 1.0 ms).

## Testing

* Host tests: `test_options`, `test_options_config`, `test_live_settings`, `test_feature_report`, `test_settings_menu`, `test_scene_lights`, `test_view_reconstruct`, `test_gbuffer`, `test_pipeline_keys`.
* GPU: `renderer_effects_smoke [--validation] [--aniso] [--smaa] [--gbuffer] [--passthrough] [--gtao] [--contact] [--ppl]` (every section when no flag), `renderer_upscale_smoke`, `renderer_smoke`. They are not part of ctest; run them from `build/renderer` (with `VK_LOADER_LAYERS_DISABLE=~implicit~` for `--validation`). `UC_SMOKE_DUMP=<dir>` saves the frames of the sections.
* Live: `python -m uctest run fight --game-arg=--ambient-occlusion --game-arg=high --game-arg=--profile` prints `[gpu] passes: …` per second; the frame log has `gpu_upscale_ms`, `gpu_smaa_ms`, `gpu_occlusion_ms`.
* Debug switches: `UC_LIGHT_STATS=<file>` (lighting per callsite), `UC_SKIP_CALLSITES=<hex>,…` (leave draws of a guest callsite out).
