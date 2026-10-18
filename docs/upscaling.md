# Upscaling and frame generation (DLSS 4.5, FSR 3.1)

Design and plan: `docs/superpowers/specs/2026-10-18-dlss-fsr-design.md`, `docs/superpowers/plans/2026-10-18-dlss-fsr.md`; scene boundary and vectors: `docs/spikes/S2-motion-vectors.md`.

## What the player gets

| Setting | Values |
|---|---|
| Upscaling (launcher, `graphics.upscaler`, `--upscaler`) | `off`, `dlss`, `fsr` (`blit` is a test-only linear scaler, rejected in the config) |
| Quality (`graphics.upscale_mode`, `--upscale-mode`) | `native` (DLAA / FSR Native AA, 1x), `quality` (1.5x), `balanced` (1.724x), `performance` (2x), `ultra_performance` (3x) |
| DLSS presets | K for native, quality, balanced; M for performance; L for ultra performance |
| Sharpening (`graphics.sharpening`, `graphics.sharpness` 0.01..1, `--sharpness`, `--no-sharpening`) | RCAS after the upscale: our own pass for DLSS, FSR's built-in one for FSR |
| Frame generation (`graphics.frame_generation`, `graphics.frame_generation_multiplier`, `--frame-generation 0|2..6`) | DLSS-G with DLSS (2x on RTX 40, up to 6x on RTX 50), FSR 3.1 FG with FSR (2x) |

The frame rate limit sets the game step and the rendered frames; with frame generation the displayed rate is the limit times the multiplier (the launcher shows it).
Deterministic runs (`--deterministic`, demos, `det_check`) switch frame generation off and are bit-identical to a run without upscaling.
The launcher asks `uc.exe --features` (a JSON line: `dlss`, `dlss_g`, `fsr`, `fsr_fg`, `dlss_g_max_multiplier`, reasons) which entries to offer; the same files the game loads answer.

## How it renders

* The 3D scene of a guest frame goes to internal targets (RGBA8 colour, D32 depth, RG16F vectors) at output size divided by the ratio; the first packet after a perspective draw that is followed by a copy covering the scene viewport ends it (`gx::SceneBoundary`, 54 recorded frames in `tests/data/scene_boundary`).
* Motion vectors come from `gx::MotionTracker` (draws paired by callsite, mesh and occurrence; previous palettes and projection); jitter is Halton(2,3) added in the vertex shader of the `smoke.motion.*` variants.
* The upscaler (`renderer/dlss_upscaler.cpp`, `renderer/fsr_upscaler.cpp`) writes the output-size EFB, the post chain and the HUD draw on top at output resolution, so the HUD is never upscaled.
* DLSS-G (`dlss_frame_generation.cpp`) and FSR FG (`fsr_frame_generation.cpp`) get the HUD-less frame (blitted before the HUD packet at back-buffer size, two buffers for FSR) so the HUD is not interpolated. Menus, movies and black frames switch generation off.

## Runtime files

Next to `uc.exe` (staged into `dist`, licences in `dist/licenses`): `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlss.dll`, `nvngx_dlssg.dll`, `NvLowLatencyVk.dll` (Streamline 2.14.16, signature checked before loading), `amd_fidelityfx_vk.dll` (FidelityFX SDK 1.1.4, signed). `cmake/upscaler_sdks.cmake` downloads both SDKs (SHA256 pinned) or takes them from `build/sdk_downloads`.
Without the DLLs or on a GPU that lacks a feature the game logs why and starts without it.

## Testing

* `renderer_upscale_smoke [--validation] [--dlss] [--fsr]`: internal scene and sharp HUD, vectors, scale change, RCAS, and a still scene that must converge to the exact edge coverage (jitter sign, vectors, per-frame target clear): DLSS quality 19, DLAA 12 (limits 25 / 16); FSR quality 11, native 4.
* Unit tests: `test_upscale_math`, `test_scene_boundary`, `test_motion_tracker`, `test_pipeline_keys`, `test_feature_report`, `test_options*`, `test_launch`.
* Live: `python -m uctest run fight --game-arg=--upscaler --game-arg=dlss|fsr --game-arg=--upscale-mode --game-arg=quality [--game-arg=--frame-generation --game-arg=2]`. `UC_CAPTURE_BURST=N` takes N consecutive captures after the measurement (ghosting and smear checks).
* Streamline's frame generation stops while its window is not the foreground window ("DLSS-G disabled: window not focused", inside the signed DLL). `UC_FG_UNFOCUSED=1` redirects the `GetForegroundWindow` import of `sl.common.dll` to the game window instead of focusing it; the harness sets it whenever `--frame-generation` is on. FSR FG has no such rule.
* `UC_SL_VERBOSE=1` prints Streamline's whole log; `UC_SCENE_DUMP=<file>|<every>|<frames>` records scene packets for the boundary tests.

## Licences

The game ships NVIDIA's runtime DLLs together with NVIDIA's licence texts (`licenses/Streamline`), names the components and carries the trademark line in `THIRD_PARTY_NOTICES.md` (installed next to `uc.exe`); nothing is added to the splash screen, the launcher or any credits screen. FidelityFX is MIT (`licenses/FidelityFX`).
Known gap, accepted by the owner: the DLSS SDK licence (`nvngx_dlss.license.txt`, exhibit on NVIDIA marks) asks for the NVIDIA marks on splash screens, in the about box and in credits, and for a notice to NVIDIA before a commercial release. This port is free and non-commercial; revisit if that changes.
