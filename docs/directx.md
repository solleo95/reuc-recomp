# DirectX renderers

Choose **Vulkan**, **DirectX 11** or **DirectX 12** in the launcher's graphics settings. The choice is saved as `graphics.renderer` (`vulkan`, `dx11`, `dx12`). `uc.exe --renderer dx11` and `uc.exe --renderer dx12` override the saved choice. Vulkan remains the default. Changing the API requires restarting the game.

Each backend consumes the same GX packets and shared Slang shader algorithms. DirectX 11 uses native D3D11 resources and DXBC; DirectX 12 uses D3D12 queues, fences, descriptors and DXIL. Device initialization failures identify the selected API.

## Features

| Feature | DirectX 11 | DirectX 12 |
|---|---|---|
| GX rendering, textures, EFB copies, depth/blend/logic operations | Supported | Supported |
| Menu, prompts, reticle, screenshots, render scale | Supported | Supported |
| Bloom, GTAO, contact shadows, shadow maps, per-pixel lighting, motion blur | Supported | Supported |
| SMAA and sharpening | Supported | Supported |
| DLSS Super Resolution / DLAA | Supported on compatible NVIDIA GPUs | Supported on compatible NVIDIA GPUs |
| FidelityFX Super Resolution | Supplied SDK has no D3D11 backend | Supported |
| DLSS / FSR Frame Generation | Supplied SDKs require D3D12 or Vulkan | Supported on compatible hardware |
| Ray tracing / path tracing | D3D11 has no DXR API | Requires DXR 1.1 and Shader Model 6.5 |
| DLSS Ray Reconstruction | Requires hardware ray tracing | Supported with DLSS and an active RT effect |
| DLSS 5 Neural Rendering | Supplied runtime reports unsupported | Supported when the user's runtime accepts this GPU |
| HDR10 | Implemented; requires HDR-enabled display | Implemented; requires HDR-enabled display |

With **DirectX 11** the launcher and the in-game menu hide what the API cannot do instead of greying it out: ray tracing / path tracing and its quality, FSR, frame generation and DLSS 5. A saved choice of those is dropped by the renderer (it never reaches the D3D11 backend), so switching the API in the launcher cannot crash the game; the choice returns when Vulkan or DirectX 12 is selected again, unless the launcher saved over it.

The launcher queries the selected API's actual device and SDK support. For diagnostics, use `uc.exe --features --renderer dx12` (or `dx11`). Feature availability also depends on the installed driver and runtime DLLs. The optional `nvngx_dlssnr.dll` belongs beside the game executable; the build does not distribute or copy this user-supplied runtime.

## Build and verification

`build.bat --no-install` builds both native backends, compiles the shared shaders for all APIs, runs CTest and stages `dist`. The Vulkan SDK remains a build dependency because it supplies Slang and the Vulkan reference backend. The Windows SDK supplies the DirectX headers and DXBC compiler; DX12 shaders use Slang's DXIL output.

GPU fixtures are separate executable targets under `build/renderer`:

```text
native_renderer_smoke.exe vulkan|dx11|dx12
native_upscale_smoke.exe vulkan|dx11|dx12 dlss|fsr|blit
native_frame_generation_smoke.exe dlss|fsr
native_rt_smoke.exe vulkan|dx12 [rr]
native_dlss5_smoke.exe [--runtime <directory>]
native_dlss5_renderer_smoke.exe
```

Unsupported API/SDK combinations are not test targets. Fixtures use a 1280×720 window or a headless device. Frame-generation testing uses the existing `UC_FG_UNFOCUSED=1` harness hook. Live game runs and performance comparisons use `scripts/uctest`, isolated saves, unlocked FPS and the deterministic `fight2` demo; see [the test protocol](testing/test-server.md).

Final source/shader compilation succeeded, and sequential CTest passed 202/202 after review fixes. Parallel CTest repeatedly hit an existing Windows sharing violation (error 32) in `test_update_process`; the same test passes alone and in the sequential suite. GPU regressions additionally cover depth-NEVER destination alpha, nonresident lost-copy fallback, mid-frame RT toggles/cancellation, latest input callbacks, and resize recovery.

SR/RR swapchains also use Streamline's DXGI proxy so the SDK observes Present and retires resources with the corresponding lifetime; see NVIDIA's [integration guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideManualHooking.md). The SDK fixture now warms up real DLSS SR in three modes before recreating the renderer with FG. An earlier game run failed during `slSetD3DDevice`; its exact SDK cause remains unproven. After correcting the missing Present hook, three consecutive full game-setting sequences (DLSS/FSR/FG/RT) passed, and separate DX11/DX12 SR fixtures retained byte-identical Vulkan reference captures. SDK shutdown failures are now logged explicitly.

Synthetic captures verify identical GX, bloom, SMAA, sharpening, contact-shadow and shadow-map output. DLSS captures match Vulkan byte-for-byte at native, quality and performance settings; FSR differences are at most one 8-bit level in those fixtures. RT uses the same sampling and denoising algorithms; compare matching frame sequences when evaluating temporal output.

A deterministic gameplay capture (`fight2`, presenting frame 4000, packet 70006) is byte-identical between DX11 and DX12. Against Vulkan, 592 of 921,600 pixels differ by more than two 8-bit levels; the mean absolute RGBA difference is 0.00354 levels. Ten isolated pixels differ by more than 20 levels, with a maximum of 71. Disabling anisotropy and separately bypassing native viewport compensation did not remove those outliers. Their exact cause remains unisolated, so gameplay output is not claimed to match Vulkan byte-for-byte.

Native telemetry reports total GPU time and CPU submit/present time. Individual GPU pass timings currently remain available only on Vulkan.

## GPU load and live switching

The ubershader (TEV words read from a constant buffer) costs about 2.5x the GPU time of the specialized shaders Vulkan builds from specialization constants. DXBC has none, so `renderer/native_tev.cpp` compiles `smoke.spec.hlsl` / `smoke.motion.spec.hlsl` (Slang output of `smoke.slang -DUBER -DUC_SPEC`, which keeps `UC_TEV(i)` calls) with `D3DCompile`, a table of the draw's TEV words in front, on background threads (~0.1 s, 8 KB each). Until a shader is ready the draw uses the ubershader; DirectX 12 builds the matching DXBC PSO on the same threads. Results are cached in `data/dx_tev_cache.bin` and the keys of `pipeline_keys.seed` / `.bin` are compiled ahead. Logic-op draws keep the ubershader. `UC_DX_SPEC_SYNC=1` compiles inside the draw (tests).

Summed GPU time per presented frame, same gameplay spot, 1280x720 window, upscaling off (`gpu_sum_ms` column of `frames.csv`): Vulkan 2.03 ms, DirectX 11 2.05 ms (was 5.24), DirectX 12 2.4 ms (was 5.24). Path tracing: Vulkan 5.9 ms, DirectX 12 about 8 ms; its remaining gap is CPU time of the renderer thread (acceleration structure and RT bookkeeping per draw).

With the specialized shaders both APIs are limited by the CPU time of the renderer thread, not by the GPU (about 35% GPU utilization in the 1280x720 harness). Both skip bindings a draw would only repeat (pipeline, render targets, viewport, buffers, tables). Frame interval at the owner's settings (per-pixel lighting, no path tracing): Vulkan 5.9 ms, DirectX 11 5.8 ms (was 7.2), DirectX 12 6.1-6.3 ms. A DirectX 12 draw costs about 0.7 us of CPU per draw call sequence in the driver (`ExecuteCommandLists`, `Present` through the Streamline proxy about 0.45 ms), which is what is left against Vulkan.

`uc.exe --features` ends the process right after printing its report: unloading the NGX/Streamline runtimes took 8 s or more (43 s on the first run after an update), and the launcher and the in-game menu wait for the process, so the DLSS/SMAA entries stayed grey with "Checking this PC" and a DirectX 11 query queued behind a DirectX 12 one.

Streamline is loaded at start for every run on both APIs, as on Vulkan, so DLSS, Frame Generation and Ray Reconstruction switch live. Before, the first DLSS or FG pick rebuilt the renderer: a 2.3 s freeze on DirectX 12 (now 0.07-0.12 s). FSR still creates its context on the first pick (0.4-0.5 s). The ray tracing pipelines are built on a background thread at start, so the first path-traced frame does not wait for a cold driver cache. DirectX 12 keeps 16 submissions in flight (was 8) and skips pipeline, target, viewport and buffer binds a draw would repeat.

## Measured performance

On a Ryzen 9 7950X / GeForce RTX 4080 (Windows build 26200, driver 32.0.16.1714), three alternating `fight2` runs at 1280×720, render scale 1, bloom enabled, upscaling/sharpening disabled and unlocked FPS produced:

| Backend | Mean wall time for 20 game seconds | Paired difference from Vulkan (95% CI) |
|---|---:|---:|
| Vulkan | 15.968 s | Reference |
| DirectX 11 | 17.768 s | +11.3% [+6.9%, +15.7%] |
| DirectX 12 | 17.782 s | +11.4% [+5.6%, +17.1%] |

Every run executed 2,608,931 draws. Both native APIs remain slower than Vulkan in this CPU-heavy scene. These are deterministic replay timings, not a claim about live gameplay FPS or other hardware.

With `--ray-tracing --rt-quality high` (path tracing), three alternating pairs measured Vulkan at 17.720 s and DX12 at 23.836 s: DX12 requires 34.5% more wall time, with a paired 95% confidence interval of [+28.5%, +40.5%]. Fence-safe temporary-resource reuse removed the initial DX12 allocation bottleneck (162.767 s in the first screening run); the remaining RT overhead is still measurable.

Separate live fights at speed 1 averaged 135.9 FPS on DX11 and 133.6 FPS on DX12 in active gameplay. These live runs are not paired benchmarks. Harness autoplay reached the chapter-complete screen on DX11 and on DX12 with high-quality path tracing, with 110 kills and clean shutdown in each run. Controls and live graphics-setting changes also passed on both APIs.

The [implementation plan](superpowers/plans/2026-10-27-directx.md) records validation and review status. Physical HDR10 output remains unverified: the test display had Windows HDR disabled. HDR toggling and SDR readback passed; an HDR-enabled display is required to verify emitted HDR10 output. Other GPU vendors were not tested.
