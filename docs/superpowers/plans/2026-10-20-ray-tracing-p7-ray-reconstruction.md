# Ray tracing phase 7: DLSS Ray Reconstruction

> Executed natively, test first, one fresh-context review at the end. Design: `docs/superpowers/specs/2026-10-19-ray-tracing-design.md` ("Denoising", RR inputs); Streamline guide `build/sdk/streamline/docs/ProgrammingGuideDLSS_RR.md`. The owner asked for it after the five effects; per the design it is not a separate option: it runs automatically when it can.

**Goal.** With DLSS selected, a GPU that supports Ray Reconstruction and at least one ray-traced effect active, one RR evaluation replaces both the own denoiser of the effects and DLSS Super Resolution for that frame: the effects composite their NOISY signals, RR denoises and upscales. Otherwise nothing changes (own denoiser, DLSS SR, or other upscalers).

## What the guide requires, and what we have

| RR input | Source here |
|---|---|
| Noisy colour (`kBufferTypeScalingInputColor`) | the scene colour with the effects composited from their undenoised signals |
| Depth, motion vectors | the scene depth and motion targets, as for DLSS SR |
| Diffuse albedo (`kBufferTypeAlbedo`) | the RT albedo target (stage-0 texture x material colour), RGBA8 |
| Specular albedo (`kBufferTypeSpecularAlbedo`) | new: from the record's reflectivity with the guide's `EnvBRDFApprox2` |
| Normals + roughness (`kBufferTypeNormalRoughness`, packed) | new: view-space normal from the octahedral G-buffer, roughness from the record's reflectivity |
| Output | the EFB, as for DLSS SR |

## Calls made here

1. **LDR in HDR mode.** The guide says RR is HDR-only (`colorBuffersHDR` must be true); the game's colour is RGBA8 in display range. It is handed over as is with pre-exposure 1: the denoiser works on the numbers it gets, and the output goes back into the same RGBA8 EFB. If the picture looks wrong the first thing to try is the exposure; this is checked live (Task 40).
2. **A frame-by-frame choice.** `use_rr = DLSS selected && RR supported && an RT effect ran this frame && no camera cut gap`. Both features are loaded with Streamline; RR options are set on the viewport when the choice changes (mode off for SR frames). RR needs a fixed input size (no dynamic resolution): the renderer's internal size is fixed per setting already.
3. **No specular hit distance / specular motion vectors.** They are optional (RR works without; the reflection signal is a colour difference, not a hit-distance carrying radiance); reflections benefit least. Deferred.
4. **Presets.** `ePresetD` (the guide's sample), all modes.
5. **Not with other upscalers.** FSR, SMAA and no upscaler keep the own denoiser.

## Tasks

### Task 36: host math (`renderer/rr_math.h`, `tests/test_rr_math.cpp`)
`rr_roughness(reflectivity)`, `rr_specular_albedo(f0, roughness, n_dot_v)` (the guide's function), `rr_use(selected, supported, effects_ran)`.

### Task 37: input pass (`renderer/rr_inputs_pass.*`, `shaders/rr_inputs.slang`)
Compute: G-buffer + material index + records -> normal/roughness RGBA16F and specular albedo RGBA8 at the scene extent. Smoke: a fixture with a known normal and reflectivity.

### Task 38: Streamline and the upscaler
Load the RR plugin with the main instance (retry without it when slInit fails), `ray_reconstruction_supported`, `slDLSSDSetOptions`, tags, evaluate in `DlssUpscaler`; `UpscaleInputs` gains the RR images and flag.

### Task 39: renderer integration
Effects composite the noisy signals when RR runs; the inputs pass; the images; the decision; the HUD/Frame generation paths unchanged. Smoke `--rt-rr`: DLSS + RR on a shadow fixture: RR ran, the shadowed floor is darker than the lit one, finite output, validation clean; the same without RR.

### Task 40: measure, live pairs (own denoiser vs RR), docs (S14), deploy.
### Task 41: one fresh review, fixes test first.
