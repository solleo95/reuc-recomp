# Native Vulkan foundation — first game picture still pending

2026-10-12. Current branch `codex/gx-vulkan`.

## Verified

- CPU regressions: 50/50 tests pass after native finish/resource recording, overflow recovery, texture decoding and offline resource audit. See `G2-textures.md` and `G3-scene-progress.md` for limits.
- Vulkan synthetic smoke on NVIDIA RTX 4080: literal pixel checks, resize, zero extent/restore, two in-flight slots, ordered one-time completions, invalid index rejection, owner-thread rejection, shutdown with GPU work. Khronos validation: zero errors in the isolated test process.
- Synthetic attachment readback inspected: `build/renderer/renderer-smoke.png`. Native WIC encodes the captured RGBA pixels. This is a colored triangle, not game imagery.
- Readback runs on demand; vertex/index allocations reuse each slot's capacity. No full-device wait on ordinary draws; resize/shutdown retire presentation resources through maintenance1 fences.
- Actual game runtime, both headless and SDL/Vulkan window modes: three-second controlled stop exits 0, joins guest/file workers, 179/181 retraces, 13 opens, 15 reads, zero unmapped MMIO.
- Latest `build.bat` stages dist and copies to the approved S: folder. Copied exe verified from `C:/Windows/Temp`: six-second validation run exits0, 357 native submissions/completions, all guest/file workers joined, zero draws and zero unmapped MMIO. ReShade disabled only in this test child process.

## Environment

The default validation run reports an installed ReShade implicit layer whose `ReShade64.dll` fails initialization (1114). A child-process-only `VK_LOADER_LAYERS_DISABLE=VK_LAYER_reshade` isolates that broken layer; other overlays remain enabled. Khronos validation remains explicitly enabled. No machine-wide registry, DLL or driver settings changed. Loader also warns about duplicate WEOBS layer manifests.

## Remaining acceptance

Native copies, actual draws, materials/textures/matrices, recognizable game screen, 1080p/4K internal targets separate from window size, and installed game-image verification remain pending. Native GPU finish advances the bootstrap: six seconds, 349 submissions/completions, zero GXBegin draws. Trace audit checks state/display-list/input paths; retraces and clear-only submissions remain separate from game-image evidence.

Current renderer requires surface/swapchain maintenance1 for verified presentation-resource retirement. A fallback for older Vulkan 1.3 drivers remains unimplemented; do not claim universal Vulkan 1.3 compatibility.
