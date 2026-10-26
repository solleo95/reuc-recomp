# Upscaler integration performance

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans. Preserve all earlier uncommitted changes. No commits.

**Goal:** Optimize DLSS and FSR integration without changing presets, input/output resolution, sharpening or generator settings; compare quality and FPS against current integration and Off.

**Architecture:** Remove measured allocation/lookup work from motion tracking and prove identical matching, camera cuts and rollback. Investigate avoidable GPU copies/attachment/synchronization work separately, retain only measured wins with pixel/motion/validation coverage. SDK neural reconstruction remains unchanged.

**Tech Stack:** C++20, Vulkan 1.3, Streamline DLSS, FidelityFX FSR, Python uctest.

**Spec:** User request in this conversation: keep presets, optimize both integrations for quality and FPS, compare current preset and disabled DLSS/FSR.

## Constraints

- Preserve K/M/L mapping, FSR flags, quality-mode ratios, shader arithmetic and native HUD resolution.
- Game runs use harness, isolated saves/config, unlimited FPS and window 1280x720. Internal render scale 2 for 2560x1440 scene output.
- Reuse current checkout because this continues its existing uncommitted implementation; investigations are read-only. Installed release remains baseline until candidate is verified.
- Baseline: installed uc.exe SHA-256 66E2E00405DC3A0A480BA72ED30F80002BC1D4AED383F7170AC87BDE55F34E58, staged as dist/uc_integration_before.exe.
- Judge performance with identical-work demo upscaler-cost; one round screens >5%, three alternating rounds resolve smaller changes. Compare baseline/candidate DLSS and FSR plus baseline/candidate Off; quality captures/regressions must retain temporal stability and reconstructed scratch quarter.

## Review focus

Draw identity includes callsite, mesh/vertex count and occurrence; equal-size geometry must choose the same camera reference. Retry rollback must release only retried records. Pool/resource lifetime must outlive its containers. GPU barriers and copy lifetimes must remain valid for HUD, 3D markers, non-clearing copies, FG and switching. Do not replace actual reconstruction with a cheaper effect or claim CPU-bound FPS gains from GPU cost alone.

## Tasks

- [ ] Stage baseline and add allocation/matching tests for warmed motion tracking. Files: renderer/motion_tracker.h/.cpp, tests/test_motion_tracker.cpp. Watch allocation regression fail, then use standard-library storage reuse; preserve deterministic equal-size camera selection. Measure isolated tracker and end-to-end costs.
- [ ] Investigate and test a concrete GPU integration saving. Files: renderer/vulkan_renderer.cpp/.h and tests/renderer_upscale_smoke.cpp only if evidence supports a change. Candidate areas: unconditional output-depth upscale, repeated copies, redundant render-pass barriers. Preserve pipeline and image quality; reject hypotheses that fail GPU validation/pixels or do not win.
- [ ] Compare quality on reproducible scene input/current settings and against native edge coverage; inspect real gameplay captures. Existing renderer_upscale_smoke and harness helpers should be reused. Only add a small test/helper for missing evidence.
- [ ] Run identical-work comparisons: current vs optimized DLSS/FSR, same presets/modes; both vs Off. Repeat meaningful/borderline changes with three alternating pairs. Run full Release, 115 tests, GPU validation/FG switch and live harness checks. Record actual result, including any unmet native-FPS target.
- [ ] Final review; resolve actionable findings. Install only verified beneficial release in <install dir>, preserving config/saves and comparing SHA-256. Archive benchmark executable out of dist before install.

## Investigation ledger

- Prior GPU timestamps: Off 1.851 ms/frame; DLSS Ultra Performance input853x480→2560x1440 presetL, total2.226 ms, evaluation0.775, complete resolve0.840. Motion CPU about0.3 ms/frame. Lower input resolution is active; integration needs measurable savings.
- Current motion tracker uses std::map for previous/current/occurrences, clears and moves containers each frame. Preserve ordering-sensitive largest-mesh tie explicitly if changing storage.
- CPU RED: constructor accepting a counting memory_resource absent; new warmed-allocation test did not compile. GREEN after PMR pool/hash maps and swap/clear: existing matching/cut/rollback/ownership tests plus deterministic tie and no-new-upstream-allocation after warmup pass.
- GPU equivalence candidates: skip previous-transform lookup/palette upload/vertex math when canonical motion_write_mask is zero; preserve tracker records. Bind motion MRT only on first clear or when a pass writes it. Skip empty EFB render pass when EFB is already initialized. Existing pixels/motion/restore regressions cover invariants; added explicit non-depth overlay preservation and empty post-boundary packet checks. Performance benchmark is the acceptance gate for these equivalence changes.
- First GPU validation process hit an access violation inside Streamline initialization before exercising changes; identical clean retry passed. This intermittent SDK initialization failure also occurred before this plan; do not mask validation errors.
- Quality follow-up found existing scene sampler LOD bias created only once from startup mode. Mode changes retain stale bias; investigate regression before deciding whether to fix. Presets themselves remain untouched.
