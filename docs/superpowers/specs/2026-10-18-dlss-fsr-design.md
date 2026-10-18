# DLSS 4.5 and FSR 3.1 upscaling and frame generation

Status: design, branch `main`. Sub-project 6 of `docs/superpowers/specs/2026-10-12-uc-recomp-design.md` (§7 frame structure, §8 upscalers); this document replaces §8's details where they differ. The frame interpolation experiment (`experiment/fps-interpolation`) does not exist for this work: do not build on it, do not design around it.

## Intent

The owner wants upscaling and frame generation that behave as in a current PC game: pick DLSS or FSR and a quality mode, the 3D scene renders at the lower internal resolution of that mode and is reconstructed to the output resolution with no artifacts, no blur and no ghosting. Frame generation multiplies the presented frame rate. The owner plays on an RTX 4080 at 2560x1440 240 Hz.

## Versions (checked 2026-10-18)

| Technology | Source | Notes |
|---|---|---|
| DLSS Super Resolution 4.5 | NVIDIA Streamline SDK 2.14.16 (released 2026-10-16), DLSS SR plugin | presets K, L, M; Vulkan supported |
| DLSS Frame Generation | same Streamline, DLSS-G plugin | Vulkan: fixed multiplier only, no Dynamic MFG, no VSync with FG (D3D12-only, `ProgrammingGuideDLSS_G.md` §6.3, §22.5). Reflex is mandatory with DLSS-G |
| FSR 3.1 upscaler + FSR 3.1 frame generation | AMD FidelityFX SDK v1.1.4 (FFX API, Vulkan backend, Vulkan frame interpolation swapchain) | owner's choice. FSR SDK 2.x (FSR 4 / FG 4) is DirectX 12 only and FSR 4 needs Radeon RX 7000/9000, so it is out |

If a newer Streamline or DLSS DLL appears before the build starts, take the newest; the presets below stay.

## Quality modes

The same five modes for both upscalers. Internal resolution per axis = output / ratio.

| Mode | Ratio | DLSS preset | FSR 3.1 mode |
|---|---|---|---|
| DLAA / Native AA | 1.0 | K | NativeAA |
| Quality | 1.5 | K | Quality |
| Balanced | 1.724 | K | Balanced |
| Performance | 2.0 | M | Performance |
| Ultra Performance | 3.0 | L | UltraPerformance |

The DLSS presets per mode are the owner's decision and are fixed (not user-selectable).

Output resolution is the extent the existing render scale option selects (`graphics.render_scale`, `renderer/render_scale.h`: monitor, 1x, 1.5x, 2x, 3x of 720p). The mode only lowers the internal resolution of the 3D scene. (Builder's call: render scale stays the "output" setting, as nothing else in the launcher describes output resolution.)

## Sharpening (RCAS)

A sharpness slider from 0.01 to 1.00 in steps of 0.01, plus an on/off toggle (builder's call: the toggle, since the slider has no zero). It applies to both upscalers (owner's choice):
- FSR: the RCAS built into FSR 3.1 (`enableSharpening` + `sharpness`).
- DLSS: our own RCAS pass after the DLSS output, using AMD's RCAS shader (MIT, FidelityFX), so the same value looks the same on both.

Sharpening applies to the upscaled scene before the HUD, so the HUD is never sharpened. With upscaling off the controls are disabled.

## Frame generation

- DLSS-G (NVIDIA RTX 40 and newer) or FSR 3.1 FG (any GPU that runs FSR 3.1), chosen with the upscaler: DLSS upscaling pairs with DLSS-G, FSR with FSR FG (builder's call: no cross pairing, it doubles the integration surface).
- Multiplier: the values the runtime reports (`DLSSGState::numFramesToGenerateMax`; RTX 4080 gives 2x, RTX 50 up to 6x). FSR 3.1 FG is 2x.
- **Frame limit with FG (owner's decision):** the existing frame limit keeps its meaning, the game step and the rendered frame rate; the presented rate is limit × multiplier. Limit 120 with 2x shows 240. Unlimited stays unlimited for rendered frames.
- Reflex is on whenever DLSS-G is on (Streamline requirement). No user toggle (builder's call).
- VSync is not available with FG on Vulkan; the present mode stays mailbox/immediate as today (FIFO caused 9..12 ms spikes, see the micro-stutter notes).
- The HUD is not generated from interpolated scene data: pass the HUD-less color (and UI color/alpha where the SDK takes it) so the generated frames keep the HUD sharp and correctly placed.
- Movies and full-screen video, menus without a 3D scene and camera cuts must not produce smeared frames: signal reset / skip generation at those points.

## Frame structure (the core of the work)

Today every packet draws into one EFB target (`efb_`, internal resolution from render scale) in `VulkanRenderer::render` (`renderer/vulkan_renderer.cpp`): render-to-texture passes with EFB copies, the 3D scene, the game's post chain (bloom: full-screen quads at callsite `0x800421B8`, `renderer/post_effects.h`), the HUD, native prompts and the late-aimed reticle (`renderer/late_aim.h`), then the present copy to the swapchain.

With an upscaler on:
1. The 3D scene (and the render-to-texture passes it consumes) renders at the internal resolution into a scene target with depth and motion vectors, with a sub-pixel jitter added to the projection of scene draws only (Halton 2,3 sequence, phase count per the SDK's recommendation for the ratio).
2. At the end of the 3D scene the upscaler reconstructs color to the output resolution, then RCAS if enabled.
3. The game's post chain, the HUD, prompts and the reticle draw at the output resolution on top of the upscaled scene, unjittered.
4. Present as today; frame generation hooks in at present.

The builder decides how the scene/HUD boundary is found (e.g. the first post-chain or orthographic UI draw after the last perspective scene draw, a per-callsite table, or both). The gx-vulkan plan warns that an orthographic projection alone does not prove a draw is HUD; the boundary has to be proven on recorded frames across scene types (gameplay, menus, cutscenes, the train, bosses). Frames with no 3D scene render directly at output resolution.

Texture sampling in scene draws gets a negative mip LOD bias of log2(internal/output) - 1 (the DLSS and FSR guides' value, otherwise textures look blurred).

Render-to-texture passes (EFB copies used as textures) that feed the scene render at the internal resolution scaled consistently, so copies keep their meaning; copies of the final scene used by the post chain read the upscaled result. The builder maps which copies are which.

## Motion vectors and depth

Motion vectors are the main quality risk: wrong or missing vectors are exactly the ghosting the owner rejects.

- Per draw, the vertex shader computes the current and previous clip-space positions; the previous one uses the previous frame's projection and position/normal matrix palette for the same draw. Skinned meshes use the per-vertex matrix rows (`GpuMeshVertex::row`, palette blocks), so the previous palette is needed per draw.
- Draw identity across frames: callsite + mesh pointer + order among draws with the same key (the approach `UC_PACKET_MOTION` in `game/packet_dump.h` relies on). Non-mesh draws (particles, CPU-built geometry) pair by callsite, order and vertex count; when unpaired, they get camera-only motion. Textures animated by texture matrices (water, scrolling surfaces) are left to the upscaler's reactive/transparency masks where the SDK offers them.
- Camera cuts and teleports trigger the upscaler's reset flag (threshold on projection/view change, tuned on recorded demos).
- Depth: the EFB depth (GX depth is reversed/ranged per viewport; the builder converts to what each SDK expects and sets the depth-inverted flags).
- Exposure: fixed (the game is LDR); pass pre-exposure 1.0.

Spike S2's acceptance rule applies: vectors are checked on a changing object and a moving camera, never assumed; zero vectors are not a success.

## Integration

- `IUpscaler` interface from the main spec (§8): init, resize, evaluate(color, depth, mv, jitter, exposure, reset), shutdown; implementations `DlssStreamline`, `FsrFfx`, none. Frame generation lives with each implementation because both hook presentation.
- Streamline on Vulkan: `slInit` before instance creation, `slSetVulkanInfo`, the device extensions and queues reported by `slGetFeatureRequirements`, the present/acquire interception and binary semaphore handshake (§16 of the DLSS-G guide). Production (signed) Streamline binaries ship in `dist`.
- FFX on Vulkan: `amd_fidelityfx_vk.dll` (FFX API) from SDK 1.1.4, upscaling context and the Vulkan frame interpolation swapchain.
- Availability: DLSS appears only on NVIDIA RTX GPUs where `slIsFeatureSupported` passes; DLSS-G only where DLSS-G is supported; FSR everywhere FSR 3.1 runs. Unsupported choices are disabled in the launcher with the reason; a config that asks for an unsupported feature falls back to off with a log line.
- Config (`config.json`, `game/options.h`, `game/options_config.cpp`) and CLI overrides: upscaler (off/dlss/fsr), quality mode, sharpening on/off and value, frame generation on/off and multiplier. Unknown keys stay preserved as today (`future_dlss` in `tests/test_options_config.cpp`).
- Launcher (`launcher/main.cpp`, enhancements column): the existing disabled "Upscaling", "Quality" and "Frame generation" controls become real; add the sharpening toggle and slider and the multiplier choice. Translations in `launcher/translations.inc`.

## Goals

- DLSS 4.5 SR and FSR 3.1 with the five modes, correct lower internal resolution per mode and correct reconstruction to the output resolution.
- DLSS-G and FSR 3.1 FG with the limit × multiplier rule.
- RCAS 0.01..1.00 for both upscalers.
- Image quality on par with current games using DLSS: no ghosting trails, no smearing, no shimmering HUD, stable thin geometry.

## Non-goals

- FSR 4 / FSR SDK 2.x, any DirectX 12 presentation path.
- Dynamic MFG and VSync with FG (D3D12-only).
- DLSS Ray Reconstruction, XeSS, NIS, user-selectable DLSS presets.
- An in-game settings menu (the launcher and config remain the place).
- The frame interpolation experiment.

## Anti-goals (owner's words)

- Artifacts of any kind, a blurred picture, ghosting.
- The game not actually rendering at the lower resolution of the chosen mode, or upscaling incorrectly (wrong aspect, offset, HUD scaled from the low resolution).

## How it is verified

- Internal resolution per mode logged and checked against the table for 1440p and 4K outputs.
- Motion vector check: reproject the previous frame with the vectors and measure the error on recorded demos (moving camera, moving characters, skinned meshes, particles); visual MV debug view.
- Captured consecutive frames in motion (fast pans, characters in front of detailed backgrounds, particles, the reticle, the HUD) for each mode, compared against DLAA and against upscaling off, reviewed for ghosting and blur.
- Frame generation: presented rate equals limit × multiplier, no HUD tearing in generated frames, no smearing at cuts and movies; frame pacing (sd of present interval) recorded.
- Performance: deterministic demo bench per `AGENTS.md` for the cost of each mode.
- Tests run through the harness (`uctest`), never system input, never real saves. FSR paths are tested on the owner's RTX 4080 (FSR 3.1 runs on NVIDIA).

## Decisions after the builder check

Answered by the project:
- Frame identity: on `main` every rendered frame is one guest step (the renderer presents what the guest submits; the limit chooses the step rate, `game/uc_main.cpp` around line 211). "Previous frame" for motion vectors is the previous guest frame. Content that did not move has zero vectors, which is correct.

Builder's calls (marked as such; the owner may overrule):
- Bloom and the rest of the post chain keep working in the game's logical 640x480 coordinates mapped to the target, as they do today at any render scale. After upscaling they run at output resolution and must look like upscaling off at the same output resolution; that is the reference.
- Ghost-free wins over "everything through the upscaler". Draws that cannot get correct vectors or depth (sky, depth-test-off, additive particles, full-screen effects inside the scene) get the SDK reactive/transparency masks first; where masks do not remove the ghosting, the builder may draw them after the upscale at output resolution (with the upscaled-scene depth reconstructed or a depth-off draw if that is what the original does). The check is the captured-frames review, not the method.
- Aim reticle with frame generation: the reticle and the HUD go through the SDK's UI path (HUD-less color + UI color/alpha for DLSS-G, the UI composition callback of the FFX frame interpolation swapchain for FSR), so generated frames never smear them. With FSR the callback runs per presented frame, so the reticle is drawn there with the freshest pointer (`late_aim`). With DLSS-G generated frames show the reticle of the latest rendered frame; Reflex keeps the latency down. Hit registration stays in the guest step.
- Deterministic runs (`--det-out`, demo, bench) support upscaling (guest state stays bit-identical; GPU output is not part of the determinism), frame generation is forced off there. The bench measures upscaler cost per mode; FG is measured in live `uctest run` scenarios.
- FSR FG present mode: the FFX Vulkan frame interpolation swapchain is asked for immediate/mailbox. If it only paces correctly with FIFO, FSR FG uses FIFO and the builder reports the frame-time measurements to the owner; DLSS-G and FG-off paths keep mailbox.
- Requirements and licensing: driver/GPU minimums come from the SDKs at runtime (`slIsFeatureSupported`, `slGetFeatureRequirements`, FFX version queries); the launcher shows the SDK's reason when a feature is unavailable. The NVIDIA RTX SDK license (Streamline/DLSS, redistribution of the DLLs with notices) and the FidelityFX MIT license are recorded in `dist` notices; the builder reads both licenses and stops to tell the owner if anything blocks shipping the DLLs.

## Left to the builder

The scene/HUD boundary detection, draw pairing details, the reset thresholds, how render-to-texture passes scale, reactive/transparency masks, how the SDKs are fetched and built (`third_party`, CMake), shader layout for the motion vector and RCAS passes, and the order of work (a minimal DLSS-G Vulkan sample first is recommended by spike S3).
