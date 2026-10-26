# Upscaler runtime fixes

> **For agentic workers:** Use superpowers:executing-plans; preserve the existing uncommitted menu work.

**Goal:** Reduce DLSS/FSR CPU overhead, switch upscalers and frame generation without rebuilding the Vulkan device, and keep frame generation visible in the launcher.

**Architecture:** Initialize optional Vulkan/Streamline capabilities once, with unsupported features falling back independently. Keep presentation backend owners alive and explicitly retire swapchains before changing backend. Preserve temporal and copy correctness from the existing GPU regressions.

**Spec:** User reports four regressions in this conversation: lower FPS with either upscaler, long upscaler switches, long frame-generation switches, missing launcher option.

**Constraints:** Harness-only game input; isolated saves/config; no commits; release to <install dir> after verification. Optional capabilities must remain optional on other GPUs.

## Tasks

- [x] Reproduce FPS loss on a fresh deterministic 3D fight. Baseline window: 1,614,225 draws; native 7.882 s, DLSS ultra 9.233 s, FSR ultra 8.793 s. Profile added CPU work before optimizing it.
- [x] Add GPU regressions for startup Off -> DLSS/FSR, family switches, FG off/on and generator switches, preserving device identity and checking pixels/completions/validation. SR validation and separate FG functional smoke pass; SDK FG validation defects remain counted and documented.
- [x] Initialize supported capabilities once in renderer/vulkan_renderer.cpp/.h; preserve modules and choose swapchain backend with safe retirement. Existing FSR context APIs suffice. Keep ordinary Vulkan presentation until FG is selected.
- [x] Remove measured avoidable motion-tracking cost in renderer/motion_tracker.cpp/.h and its renderer caller; pin matching/rollback semantics in tests/test_motion_tracker.cpp. Unit tests and focused review pass; measured motion CPU time 0.425 -> 0.303 ms/frame before removing temporary timers.
- [x] Reproduce launcher visibility/clipping and fix its smallest root cause; verify through its synthetic smoke flow. Off/DLSS/FSR selector tested from upscaling Off, including saved options.
- [x] Run full Release build, GPU regressions, overlay scenario, identical-work A/B bench and live fight; inspect native launcher/menu captures. Deploy and hash-check release files; preserve installed config/saves.

## Final evidence

- Full `build.bat --no-install`: Release/ThinLTO, 115/115 tests. Native/SR Vulkan validation and separate DLSS/FSR FG functional smoke pass. FG validation SDK defects remain visible; see docs/upscaling.md.
- DLSS Ultra Performance: 9.483 -> 9.120 s, -3.8%, paired 95% CI [-6.8%, -0.9%], three alternating demo pairs. FSR: 9.278 -> 8.516 s, -8.2%, one pair. Native candidate: 7.908 s; CPU-bound reconstruction remains slower than native. Each measured window: 1,614,225 draws.
- Overlay PASS: build/autotest/1792365991509257900. SR updates 6.8-12.0 ms, generator change 30.1 ms, repeated FG toggles 0.1-6.2 ms; no device restart. Timing excludes subsequent deferred SDK work.
- Live DLSS Quality / render-scale 2 / window 1280x720: PASS, mean 157.8 FPS, active frame-gap p99 9.0 ms; build/autotest/1792366168371421500.
- Installed 56 release files in <install dir>; every SHA-256 matched. Config and five saves unchanged. Excluded debug artifacts and archived the benchmark baseline under build/uc_perf_before_4bfe.exe.
- Final review found one refresh-target lifetime gap. Regression failed before the fix (`build/upscale-retention-red.log`); retaining the target in the submitting slot fixes it. Reviewer confirmed closure; refreshed SR validation and FG functional smoke pass (`build/upscale-retention-green.log`, `build/upscale-retention-fg.log`). Full Release rebuilt, 115/115 pass (`build/upscaler-release-final.log`), redeployed and all 56 hashes reverified. Performance/live results above precede this final lifetime guard.
- Installed uc.exe SHA-256: 66E2E00405DC3A0A480BA72ED30F80002BC1D4AED383F7170AC87BDE55F34E58. Launcher: CAB79A86E31D2C8E1096CB22C8F238834DCB9AC73E77098F4AB3D3BAE18573FE.

## Review focus

Unsupported/missing SDKs must preserve native rendering. Never mutate pipeline capabilities while background workers read them. Presentation lambdas must outlive all swapchains using them. Native mode must stop jitter and generation. Refresh only the final unchanged scene copy. Profiler output must not be mistaken for a GPU-bound speed guarantee.
