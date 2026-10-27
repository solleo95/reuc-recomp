# DirectX 11 and DirectX 12 renderers

## Intent and accepted approach

Add complete, usable DirectX 11 and DirectX 12 rendering with the same picture as the existing Vulkan renderer and measured gameplay performance. The owner selected separate native backends sharing GX processing and shader logic, and authorized staged execution without further questions on 2026-10-27. Intermediate milestones are not completion of this request.

Keep Vulkan as the reference and default. Reuse `uc_gx`, `FramePacket`, scene reconstruction, texture decoding, material state, render-scale math and Slang shader sources. Each backend owns its device, resources, synchronization, presentation and API-specific SDK integrations. Never select a different backend silently.

## Alternatives considered

1. Separate native backends with common GX/shader logic (selected): confines changes to the rendering boundary and preserves a working Vulkan reference. Some resource/pass management is duplicated.
2. A universal low-level graphics abstraction with a Vulkan rewrite: shares more GPU plumbing, but introduces broad regression risk before either new backend is usable.

## Required behavior

- Backend selection persists in graphics configuration, is available from the launcher, and has a CLI override. Invalid names fail with a useful error. Startup/device failure names the selected backend.
- Preserve GX semantics: TEV, lighting, fog, indexed transforms, texture formats/mips, alpha/depth/blend/logic operations, destination alpha, EFB copies, copy clears, scene replay and completion order.
- Preserve render scale/aspect, resize/minimize, screenshots, menu/prompt overlays, late aim, held-frame reticles, and deterministic harness behavior.
- Port the enhanced raster effects, HDR and supported upscaling/frame-generation integrations. Audit RT/path tracing and vendor SDK availability per API. An unavailable feature must be reported honestly; silently dropping visual effects does not establish parity.
- Share shader algorithms; declare API-specific resource bindings and coordinate conversion explicitly. GPU objects stay behind the backend boundary.
- Cache pipelines, immutable geometry, textures and sampler state; bound transient allocations and frames in flight. Completion means actual GPU retirement, including hidden/replay submissions.
- Fail clearly on invalid resources/device loss; teardown partially initialized backends safely. Preserve renderer-thread ownership.

## Verification and completion

Use existing synthetic renderer fixtures and deterministic captured gameplay as Vulkan references. Test both DX backends with depth/blend/TEV/EFB edge cases and compare readbacks; investigate every difference beyond documented quantization tolerance. Exercise menus, gameplay, controls, resize and backend configuration. A renderer that only clears/presents or runs a simplified shader is not complete.

Build with `build.bat --no-install`; run CTest. Live `uc.exe` runs use only `uctest`, isolated saves, mute, 1280x720 window and unlocked FPS. Performance comparisons use deterministic `fight2` bench with three alternating pairs and reported confidence intervals, followed by a speed-1 live run. Preserve a separate reference executable and matching data before staging changed binaries.

## Constraints and risks

Work in `feature/directx11-directx12`. Preserve existing changes to `renderer/late_aim.h` and `tests/test_late_aim.cpp`. Do not commit or push without explicit request. No game launches outside the harness, fullscreen tests, or real-save writes.

DirectX feature levels and SDK support differ. Establish actual device/compiler/SDK capabilities before promising enhanced-feature parity; record remaining gaps rather than relabeling a partial backend complete. The owner delegates routine implementation choices and performance tuning to the implementer.
