# Graphics effects

Design: `docs/superpowers/specs/2026-10-19-graphics-enhancements-design.md`; plan of phases 0–3: `docs/superpowers/plans/2026-10-19-graphics-enhancements-p0-3.md`; survey of the game's lighting: `docs/spikes/S6-graphics-survey.md`.

All effects live in the renderer (GPU passes recorded on the render thread); the guest threads are never touched and no effect reads pixels back. Every one is a separate option in the launcher (Graphics page) and in the in-game menu (Insert, Effects tab), saved in `config.json` and overridable on the command line. All off with anisotropy 1 is the original picture.

| Option | Config (`graphics.*`) | CLI | Values (default) | Needs |
|---|---|---|---|---|
| Anisotropic filtering | `anisotropy` | `--anisotropy` | 1, 2, 4, 8, 16 (16) | `samplerAnisotropy` (clamped to the GPU) |
| SMAA 1x | `upscaler` | `--upscaler smaa` | a value of Anti-aliasing / upscaling | the effects path |
| Per-pixel lighting | `per_pixel_lighting` | `--per-pixel-lighting` | on/off (off) | the effects path |
| Ambient occlusion (GTAO) | `ambient_occlusion` | `--ambient-occlusion` | off, low, medium, high, ultra (off) | the effects path |
| Contact shadows | `contact_shadows` | `--contact-shadows` | on/off (off) | the effects path, a directional or spot light in the scene |
| Motion blur | `motion_blur` | `--motion-blur` | off, low, medium, high, ultra (off) | the effects path; skipped on camera cuts |
| Ray tracing (master switch) | `ray_tracing` | `--ray-tracing` | on/off (off) | a GPU with ray query (`uc.exe --features`: `ray_tracing`) |
| Ray-traced shadows | `rt_shadows` | `--rt-shadows` | off, low, medium, high, ultra (off) | the master switch on; replaces the shadow map and contact shadows |
| Ray-traced local shadows | `rt_local_shadows` | `--rt-local-shadows` | off, low, medium, high, ultra (off) | the master switch on; point and spot lights |

"The effects path" = `VK_EXT_dynamic_rendering_unused_attachments` + `independentBlend` (the same the upscalers need); without it those options are greyed out with a reason (`uc.exe --features` reports `effects`, `effects_reason`, `anisotropy_max`).

## How it renders

* The 3D scene of a frame renders into the internal scene targets (colour RGBA8, depth D32, motion RG16F, G-buffer RGBA16) up to the scene boundary (`gx::SceneBoundary`). Without an upscaler the targets exist only while an effect needs them (a blit pass-through at ratio 1); switching that on or off rebuilds the targets, every other toggle is live.
* G-buffer (`renderer/gbuffer.h`): octahedral view normal, GX fog factor, flags; written by depth-writing draws only.
* At the boundary, before the upscaler: `renderer/occlusion_pass.cpp` runs GTAO (horizon search at half or full resolution, depth-aware blur) and the contact-shadow march, then multiplies both into the scene colour, faded by the fog factor.
* Per-pixel lighting is a push-constant bit in the motion shader variants: the GX lighting equations run on the interpolated view normal and position instead of per vertex.
* SMAA is an `Upscaler` at ratio 1 (compute, `third_party/smaa`); no jitter, no vectors.
* Presets (budget per effect on the owner's RTX 4080, measured with the pass timestamps): GTAO medium + contact shadows 0.30 ms and high 0.50 ms at the harness size (limits 0.4 / 1.0 ms).

## Ray tracing

Design: `docs/superpowers/specs/2026-10-19-ray-tracing-design.md`; plans `docs/superpowers/plans/2026-10-19-ray-tracing-p0-1.md` (device support, acceleration structures, material records, denoiser) and `…-p2.md` (shadows); measurements `docs/spikes/S8-ray-tracing.md`. Only the shadows work so far; reflections, lighting and ambient occlusion with ray tracing follow (their options exist in `config.json` and on the command line but have no effect and no UI).

* The master switch turns the active ray-traced options on and off; the levels are kept. An option is active when the master is on and its level is not `off`. Unsupported GPUs (no ray query, fewer than 5 colour attachments) ignore all of them (`--features`: `ray_tracing_reason`).
* Each frame's scene draws become a view-space acceleration structure (one BLAS per packet, the TLAS at the scene boundary, `renderer/rt_scene.cpp`). While RT shadows are active the scene pass also writes a per-pixel material record index and the albedo (`renderer/rt_material.h`).
* `renderer/shaders/rt_shadow.slang`: per pixel the draw's own GX lights are evaluated with the GX lighting equations, one ray query per selected light, and the factor `1 − Σ share_i·(1 − visibility_i)` is written at 1/divisor of the internal resolution; the own denoiser (`renderer/rt_denoiser.cpp`) filters it and a depth-aware composite multiplies the scene colour on lit, model-valid pixels, faded by the GX fog. Directional lights follow `rt_shadows`, point and spot lights `rt_local_shadows` (the strongest 1/2/4/8). Alpha cut-outs cast no ray-traced shadow yet.
* Presets (`renderer/rt_shadow_math.h`): the resolution divisor, rays per pixel and soft edges per level; tuned against the budget (High ≈ 100 FPS for everything ray-traced on the RTX 4080).
* While RT shadows are active the shadow map and contact shadows do not run; the launcher and the in-game menu grey their options ("Заменено трассировкой лучей") and the saved values come back with the master switch off.
* Timing: frame log `gpu_rt_as_ms`, `gpu_rt_shadows_ms`; `--profile` prints `rt_shadows`.

## Testing

* Host tests: `test_options`, `test_options_config`, `test_live_settings`, `test_feature_report`, `test_settings_menu`, `test_scene_lights`, `test_view_reconstruct`, `test_gbuffer`, `test_pipeline_keys`.
* GPU: `renderer_effects_smoke [--validation] [--aniso] [--smaa] [--gbuffer] [--passthrough] [--gtao] [--contact] [--ppl] [--rt-material] [--rt-as] [--rt-shadows] [--rt-denoise]` (every section when no flag), `renderer_upscale_smoke`, `renderer_smoke`. They are not part of ctest; run them from `build/renderer` (with `VK_LOADER_LAYERS_DISABLE=~implicit~` for `--validation`). `UC_SMOKE_DUMP=<dir>` saves the frames of the sections.
* Live: `python -m uctest run fight --game-arg=--ambient-occlusion --game-arg=high --game-arg=--profile` prints `[gpu] passes: …` per second; the frame log has `gpu_upscale_ms`, `gpu_smaa_ms`, `gpu_occlusion_ms`.
* Debug switches: `UC_LIGHT_STATS=<file>` (lighting per callsite), `UC_SKIP_CALLSITES=<hex>,…` (leave draws of a guest callsite out).
