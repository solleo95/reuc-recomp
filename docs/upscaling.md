# Upscaling and frame generation (DLSS 4.5, FSR 3.1)

The native DirectX backends share the upscaling settings and shader algorithms described here. API-specific SDK availability and fixture commands are listed in [DirectX renderers](directx.md). Vulkan-specific extension, queue and swapchain details below apply to Vulkan.

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
* Motion vectors come from `gx::MotionTracker` (draws paired by callsite and mesh, identical instances with the nearest one where the camera's last motion predicts it; previous palettes and projection); jitter is Halton(2,3) added in the vertex shader of the `smoke.motion.*` variants.
* The upscaler (`renderer/dlss_upscaler.cpp`, `renderer/fsr_upscaler.cpp`) writes the output-size EFB, the post chain and the HUD draw on top at output resolution, so the HUD is never upscaled.
* The last scene copy is refreshed from the reconstructed EFB at the boundary when no intervening draw or clear changed its pixels. The train's bloom chain saves the top-left 320x180 workspace before reconstruction and restores that exact texture at `80044340`; leaving it raw put low-resolution jittered pixels back over the reconstructed corner. Earlier scene copies keep their original contents.
* DLSS-G (`dlss_frame_generation.cpp`) and FSR FG (`fsr_frame_generation.cpp`) get the HUD-less frame (blitted before the HUD packet at back-buffer size, two buffers for FSR) so the HUD is not interpolated. Menus, movies and black frames switch generation off.

## Runtime files

Next to `uc.exe` (staged into `dist`, licences in `dist/licenses`): `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlss.dll`, `nvngx_dlssg.dll`, `NvLowLatencyVk.dll` (Streamline 2.14.16, signature checked before loading), `amd_fidelityfx_vk.dll` (FidelityFX SDK 1.1.4, signed). `cmake/upscaler_sdks.cmake` downloads both SDKs (SHA256 pinned) or takes them from `build/sdk_downloads`.
Without the DLLs or on a GPU that lacks a feature the game logs why and starts without it.

## Testing

* `renderer_upscale_smoke [--validation] [--dlss] [--fsr] [--framegen]`: internal scene and sharp HUD, vectors, scale change, live RCAS, first-off/family/off-on switches on the same device, and a still scene that must converge to the exact edge coverage (jitter sign, vectors, per-frame target clear): DLSS quality 19, DLAA 12 (limits 25 / 16); FSR quality 11, native 5. `--framegen` additionally repeats generator toggles and switches between presentation backends, checking completed frames and pixels.
* The scratch-workspace regression restores a pre-boundary quarter-scene copy with mipmaps and compares its edges to the reconstructed EFB: DLSS quality failed at 13.70 levels of error before the fix; DLSS/FSR quality/native now measure 0.00. Separate draw/clear cases prove earlier snapshots retain their original pixels. Logs: `build/upscale-restore-red.log`, `build/upscale-final.log`.
* The refreshed snapshot is retained by the refresh submission's slot, independently of its original-copy slot and guest owners. A weak-owner regression fails before that retention and passes after it (`build/upscale-retention-red.log`, `build/upscale-retention-green.log`). This prevents recycling a copy image while the refresh still uses it.
* Unit tests: `test_upscale_math`, `test_scene_boundary`, `test_motion_tracker`, `test_pipeline_keys`, `test_feature_report`, `test_options*`, `test_launch`.
* Live: `python -m uctest run fight --game-arg=--upscaler --game-arg=dlss|fsr --game-arg=--upscale-mode --game-arg=quality [--game-arg=--frame-generation --game-arg=2]`. `UC_CAPTURE_BURST=N` takes N consecutive captures after the measurement (ghosting and smear checks).
* Streamline's frame generation stops while its window is not the foreground window ("DLSS-G disabled: window not focused", inside the signed DLL). `UC_FG_UNFOCUSED=1` redirects the `GetForegroundWindow` import of `sl.common.dll` to the game window instead of focusing it; the harness sets it whenever `--frame-generation` is on. FSR FG has no such rule.
* `UC_SL_VERBOSE=1` prints Streamline's whole log; `UC_SCENE_DUMP=<file>|<every>|<frames>` records scene packets for the boundary tests.

## Live settings

RCAS enable/disable and strength travel with each reconstruction, preserving the Vulkan device, textures, pipelines and temporal history. The standalone RCAS pipeline initializes once on first use; FSR receives its sharpening settings per dispatch.
Optional Streamline features, Vulkan extensions and interpolation queues are provisioned once at startup. First enable, family switches and generator toggles preserve the device, pipelines, meshes and scene-copy textures. Quality changes rebuild the reconstruction context and targets. Native/SR presentation uses ordinary Vulkan until generation is enabled; changing generator backend retires its swapchain through its owner before selecting another. Repeated FG off/on within one upscaler family keeps the generator context and swapchain. Unsupported optional capabilities fall back independently.

On the RTX 4080 at 1440p, RCAS toggles previously restarted the renderer for 2050–2181 ms; live updates measured 0.0–0.5 ms. Repeated DLSS off/on host updates measured 8.4–12.6 ms in the live `overlay` scenario (`build/autotest/1792361229544739900/report.json`). The GPU smoke test's DLSS off/on pair, including one native present, measured 19–21 ms. First DLSS initialization remained about 2.2 s. These are host update timings; final reconstruction/context allocation can also add GPU work to the following frame.

The deterministic `fight2` comparison ran identical 1,328,859-draw windows: baseline 4.052 s, fixed 4.038 s (-0.3%, within the 5% noise floor). Its logs show no DLSS evaluation, so this comparison checks shared renderer overhead and does not establish reconstruction performance. Settled live DLSS captures for quality/performance/DLAA are in `build/autotest/1792361759797567200/diagnostic.json`.
The subsequent live heavy-fight check with DLSS quality at 2560x1440 and 2x render scale passed: 164.1 mean FPS, p5 140, active frame gap p99 8.1 ms (`build/autotest/1792362363611200400/report.json`). This is an absolute live result, not an A/B speed verdict.

The next CPU investigation uses the fresh `upscaler-cost` demo (1,614,225 draws in the measured window), which actually dispatches reconstruction. Immutable transform snapshots replace per-draw palette construction/copies; unique previous/current palettes share one upload. Instrumented motion work fell from 0.425 to 0.303 ms/frame; temporary per-draw timers were removed from the release. Reconstruction and motion vectors still cost time: on a CPU-bound scene, reducing internal pixels cannot guarantee higher FPS than native.

1280x720-window / 2x-render-scale comparisons, before the final snapshot-retention guard: DLSS Ultra Performance 9.483 -> 9.120 s (-3.8%, paired 95% CI [-6.8%, -0.9%], three alternating pairs); FSR Ultra Performance 9.278 -> 8.516 s (-8.2%, one pair, 5% noise floor). Native on the candidate was 7.908 s. Every measured window had the same 1,614,225 draws. These results reduce the regression but still show CPU-side reconstruction overhead relative to native. Logs: `build/upscaler-dlss-bench.log`, `build/upscaler-fsr-bench.log`.

The final live overlay run (`build/autotest/1792365991509257900/report.json`, PASS) measures SR host updates at 6.8–12.0 ms, first DLSS-G enable at 9.8 ms, DLSS-G repeat off/on at 1.2/0.1 ms, generator change to FSR at 30.1 ms, FSR-G repeats at 0.2–6.2 ms, and FSR-G -> Off at 22.4 ms. No device restart occurred. These measure setting application; SDK shader compilation/evaluation on the following frame is separate. All 115 tests, native/SR Vulkan validation and the separate FG functional smoke pass.

FG validation has a separate SDK limitation: the signed AMD runtime 1.0.1.41314 omits STORAGE_BUFFER from its descriptor-pool count, creates UI pipelines with lineWidth=0, and reports internal present-semaphore reuse and leaked UI resources. Streamline's FG pacer can also report internal present-semaphore reuse. These messages remain visible and counted; the FG functional run is separate from clean native/SR validation. The host supplies FFX replacement images in SHADER_READ_ONLY_OPTIMAL, as required by its API.

The launcher exposes frame generation as Off / DLSS / FSR even when upscaling starts Off. Choosing a generator enables its matching upscaler. Harness runs and deterministic replays default to a 1280x720 window, independently of render scale; a fullscreen mode requires an explicit game argument.

## GPU cost follow-up

The owner's next captures show 235 -> 214 FPS, GPU utilization 47 -> 56%, and power 124 -> 162.5 W with Ultra Performance. A separate timestamp-instrumented run reproduces the increase in GPU work; CPU limits alone do not explain it. `upscaler-cost`, render-scale 2, 1280x720 window, 2560x1440 reconstruction output, sharpening/FG off, identical 1,614,225-draw measured windows. Last six 120-present GPU blocks average native 1.851 ms/frame, Ultra Performance 2.226 ms/frame, Quality 2.341 ms/frame.

Ultra Performance actually receives 853x480 (preset L); Quality receives 1707x960 (preset K). In Ultra Performance the DLSS evaluation costs 0.775 ms, or 0.840 ms including its surrounding resolve work. Other GPU work falls to 1.386 ms, saving about 0.465 ms versus native; resolve costs more than that saving, increasing total GPU time by about 20%. The upscaler is being applied at reduced input resolution; its own compute cost outweighs raster savings in this lightweight renderer. The current integration therefore offers temporal reconstruction/anti-aliasing, not a guaranteed native-FPS improvement.

NVIDIA's recommended Ultra Performance preset L explicitly trades more performance for stability/ghosting improvements over J/K (`Streamline/include/sl_dlss.h`; upstream https://github.com/NVIDIA-RTX/Streamline/blob/main/include/sl_dlss.h). No preset or image-quality change was shipped from this investigation. Temporary timestamp instrumentation was removed; the installed release remains unchanged. Evidence: `build/dlss-gpu-cost-bench.log`, `build/autotest/demo-1792367069564/run.log` (native), `demo-1792367098019/run.log` (Ultra Performance), `demo-1792367127756/run.log` (Quality). Diagnostic frame-time comparisons include instrumentation and one run per mode; treat the GPU-stage timings as local measurements, not a universal hardware claim.

## Licences

DLSS 5 is off by default and `nvngx_dlssnr.dll` is never shipped: the player puts it next to `uc.exe`, which enables the launcher's DLSS 5 checkbox (`graphics.dlss5` in `config.json`, `--dlss5` on the command line). While the checkbox is off the DLL is not loaded and the in-game DLSS 5 tab is hidden. DLSS 5 Neural Rendering loads the local `nvngx_dlssnr.dll` through its Vulkan snippet exports, including custom unsigned builds. Put the runtime in `project/dlss5/` before building, or set `UC_DLSS5_RUNTIME`; put the selected copy beside `dist/uc.exe` too, since deployment preserves the player's DLL. The loader uses an absolute path and changes only this DLL's `GetModuleFileNameW` import to satisfy its caller-identity check, following the RenoDX direct-runtime approach. Feature creation determines GPU support because custom builds may retain the original minimum-GPU metadata. Other Streamline and NGX features retain their signature checks. Test an expected-compatible runtime with `build/renderer/renderer_dlss5_smoke.exe <runtime-directory> --require-runtime --validation`, with the same DLL beside the smoke executable and implicit Vulkan layers disabled (`VK_LOADER_LAYERS_DISABLE=~implicit~`); this checks real creation and evaluation as well as the stand-in regressions.

The game ships NVIDIA's runtime DLLs together with NVIDIA's licence texts (`licenses/Streamline`), names the components and carries the trademark line in `THIRD_PARTY_NOTICES.md` (installed next to `uc.exe`); nothing is added to the splash screen, the launcher or any credits screen. FidelityFX is MIT (`licenses/FidelityFX`).
Known gap, accepted by the owner: the DLSS SDK licence (`nvngx_dlss.license.txt`, exhibit on NVIDIA marks) asks for the NVIDIA marks on splash screens, in the about box and in credits, and for a notice to NVIDIA before a commercial release. This port is free and non-commercial; revisit if that changes.
