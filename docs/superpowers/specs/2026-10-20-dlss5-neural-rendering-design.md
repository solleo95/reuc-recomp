# DLSS 5 Neural Rendering (NGX feature "dlssnr")

## Intent

The owner wants NVIDIA's DLSS 5 Neural Rendering as its own option: a "DLSS 5" tab in the launcher and in the in-game menu (Insert), saved in `config.json`, working on RTX 20/30/40/50 and degrading to "off + a clear reason" wherever the runtime cannot create the feature. The runtime is `nvngx_dlssnr.dll`, an NVIDIA-signed NGX feature snippet. It loads **only** through the driver's NGX core (`nvngx.dll`) from the path list we give `NVSDK_NGX_FeatureCommonInfo.PathListInfo`. Nothing is patched, hooked or spoofed; the core's signature and caller checks stay in force.

## Goals / non-goals

Goals: the standard NGX route (Vulkan SDK API from the Streamline SDK's `external/ngx-sdk`, static `nvsdk_ngx_d.lib`), every setting persisted and strictly validated, a probe that tells the launcher whether the feature can be created (and why not), an NGX log file in `debug/`.

Non-goals: Streamline plugin for dlssnr (none exists), HDR output (the game is SDR), a D3D path, shipping the 254 MB DLL in git.

## Facts the design rests on

* Feature id 18 (`NVSDK_NGX_Feature_Reserved18`). Instance/device extensions come from `NVSDK_NGX_VULKAN_GetFeature{Instance,Device}ExtensionRequirements` (VK_NVX_binary_import, VK_NVX_image_view_handle, buffer device address, push descriptor, properties2).
* The DLL in `<downloads>` targets arch 0x1B0 (RTX 50); on other GPUs the core logs "snippet is using newer GPU arch" and `CreateFeature` returns `0xbad0000b`. The owner will drop in a DLL built for their GPU later. Everything must therefore be correct for an unverified network: only the parameter names (from the DLL's own strings) are followed.
* The DLL reads: `DLSSNR.Color/Depth/MVec/Output`, `*SubrectBaseX/Y/Width/Height`, `MVecScaleX/Y`, `JitterOffsetX/Y`, `Reset`, `DepthInverted`, `Intensity`, `GlobalToneStrength`, `LocalToneStrength`, `LocalStructureStrength`, `SkinStructureStrength`, `Style`, `UseAutoMask`, `MaskBlendMode`, `UICorrection`, `UI`, `UIAlpha`, `ScalingRatio`, `Width`/`Height` (create), `Hint.Render.Preset`, `Enabled`. It has **no** exposure, encoding, nits, filter or pass parameters: those belong to the host (see below).

## Pipeline (host side)

`Dlss5Pass` runs between three possible stages (setting `stage`): before the upscaler (on the jittered scene colour), after the upscaler before the HUD (default; on the EFB), at present (EFB with the HUD, just before the swapchain copy). Per run:

1. `prepare` compute: RGBA8 region -> RGBA16F bridge image at `processing_scale` x size. Applies the downsample filter (nearest / bilinear / bicubic) in the scaling domain (encoded or linear), the exposure and diffuse-white gain, and decodes the signal encoding (auto = sRGB, gamma 2.2, gamma 2.4, linear).
2. NGX `EvaluateFeature` `passes` times (ping-pong between two bridge images; depth and motion vectors are the scene targets with their sub-rectangles and `MVecScaleX/Y`).
3. `resolve` compute: bridge image -> RGBA8 region: encode back, remove the gain, upsample filter, domain.

The bridge is the only place the encoding / exposure / nits / filter / domain settings act; with defaults it is a lossless round trip apart from the network.

## Settings (`config.json` -> `"dlss5": {...}`, one table drives parsing, saving, launcher, in-game menu)

enabled (false), preset (`1`,`2`,`3`,`h`,`a`,`b`; default `1`), style 0..2, intensity, tone_global, tone_local, structure_local, structure_skin (0..1, default 1), auto_mask (true), passes 1..4, processing_scale 0.5..1, downsample_filter / upsample_filter (nearest, bilinear, bicubic), scaling_domain (encoded, linear), ui_correction (off, auto, on), exposure 0.25..4, exposure_mode (auto = 1.0, manual), diffuse_white_nits 80..400, diffuse_white_override, signal_encoding (auto, srgb, gamma22, gamma24, linear), stage (render, upscaled, present), block_hudless, debug.

My calls, flagged for the owner: the `preset` -> `Hint.Render.Preset` integer table (`dlss5_settings.h`) is a guess (the DLL falls back to its default preset for an unknown value); `style` is an index because the DLL names none; `block_hudless` skips black/fade frames; `ui_correction=on` only sets `DLSSNR.UICorrection=1` (the game has no separate UI layer to hand over); `exposure_mode=auto` leaves the picture unscaled because a display-referred SDR frame has no exposure to estimate.

CLI: `--dlss5` (enable) and `--dlss5-option key=value` (same strict parser as the config).

## Support probe and failure behaviour

`uc.exe --features` creates a throwaway instance + device with the NGX extensions, initialises NGX with the exe directory as the feature path and an app log callback, asks `GetFeatureRequirements`, then `CreateFeature` on an open command buffer. Any failure becomes a short reason (missing DLL, unsupported GPU architecture, driver too old, `CreateFeature` result code with the last NGX log line), shown as the tab's status line and tooltip; the switch is greyed (a saved "on" stays visible and can be switched off). The renderer repeats the same creation on its own device when the setting is on and drops to "inactive" with the reason on failure. No failure can crash the game.

## Build / deploy

`nvsdk_ngx_d.lib` + headers from the Streamline SDK archive already fetched. The runtime is copied from `UC_DLSS5_RUNTIME` (cache or environment variable) or `project/dlss5/nvngx_dlssnr.dll` (git-ignored) into the build tree and `dist` when present; absent, the feature reports "nvngx_dlssnr.dll missing".

## Verification boundary

Verifiable here: options/config/UI logic, parameter mapping against a fake NGX, bridge math, device init + NGX init + `GetFeatureRequirements` + `CreateFeature` failing gracefully on an RTX 4080, launcher and menu rendering, renderer smoke with validation. Not verifiable: the network's output, preset values, the evaluate path on real hardware.
