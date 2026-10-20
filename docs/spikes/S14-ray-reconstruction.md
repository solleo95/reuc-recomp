# S14: DLSS Ray Reconstruction (phase 7)

Date 2026-10-20, branch `feature/ray-tracing`, plan `docs/superpowers/plans/2026-10-20-ray-tracing-p7-ray-reconstruction.md`. RTX 4080, DLSS Quality at render scale 2 (1707x960 internal to 2560x1440 output), `fight3` replay (3243 frames), one run per setting; `UC_NO_RR=1` runs the same build with the own denoiser and DLSS Super Resolution.

## What was built

With DLSS selected, a GPU that supports Ray Reconstruction (`slIsFeatureSupported(kFeatureDLSS_RR)`), the graphics option `ray_reconstruction` on (default; `--no-ray-reconstruction`, launcher checkbox, in-game menu checkbox) and an RT effect active, the frame goes through one RR evaluation instead of the own denoiser plus Super Resolution (`renderer/dlss_upscaler.cpp`, `renderer/vulkan_renderer.cpp`): the effects composite their undenoised signals into the scene colour, RR gets that noisy colour, the depth and motion targets, the RT albedo target as diffuse albedo, and two new guides written by `renderer/shaders/rr_inputs.slang` (`rr_inputs_pass.cpp`): the view-space normal with the roughness in its alpha, and the specular albedo (the guide's EnvBRDFApprox2, `renderer/rr_math.h`). Roughness and specular colour come from the draw's reflectivity (S12); matte draws are fully rough. Preset D, packed normals, `colorBuffersHDR` true with the game's display-range colour passed as it is (the guide says RR takes HDR only; exposure 1). No specular hit distance or specular motion vectors (optional). RR is loaded with Streamline when `sl.dlss_d.dll` ships; if its plugin fails slInit the game retries without it. Frames without an RT effect, other upscalers and a GPU without RR keep the own denoiser and Super Resolution.

## What it looks like

First scene, all five effects at High, same moments with and without RR: the mean brightness is the same (34.0 and 33.7 grey levels over ten frames: the display-range input needs no exposure fix); the picture is sharper and the denoising is cleaner (frame to frame difference in motion 8.7 against 13.9). One visible change: **the glow around bright lights is gone**. The game draws its bloom as blended glow sprites inside the scene colour; RR treats them as noise or ghosting and reconstructs the lamp glass underneath (the chandelier shows its panes instead of a white blob). It is a different look (arguably cleaner), not the original's. Feeding the sprites to RR as a transparency guide (`kBufferTypeColorBeforeTransparency`) would need the scene colour split before the blended draws; deferred. The option exists for exactly this: off gives the previous picture.

## Cost (fight3, RT off 7.21 ms with DLSS)

| Setting | with RR `interval_ms` med / p95 | with own denoiser + SR | RR costs |
|---|---|---|---|
| shadows + local shadows High | 9.83 / 11.55 | 8.56 / 10.02 | +1.27 ms |
| all five effects High | 10.81 / 12.49 | 9.33 / 10.66 | +1.47 ms |

RR is heavier than Super Resolution plus the own denoiser at 1440p (the effects' own passes get cheaper: 0.38 instead of 0.93 ms for the five), so all five at High with RR run at about 93 FPS in the heavy fight (target about 100) against about 107 with the own denoiser. The option is there to trade the cleaner picture for the speed.

## Checks

`renderer_effects_smoke --rt-rr` (DLAA through DLSS, the lighting fixture): the frame goes through RR (`ray_reconstruction_active`), the floor beside the red wall gets the wall's red (87 to 140 through RR, 138 through the own denoiser), the wall stays red, validation clean. `test_rr_math` (roughness, specular albedo, the decision), `test_options*` (the option, its flag and its config key).

## Not covered

* Specular hit distance / specular motion vectors (RR's input for reflections in motion), the transparency guides, DLSS-G together with RR (frame generation was not exercised with RR frames), RR at modes other than Quality in the live game.
* The bloom change above.
