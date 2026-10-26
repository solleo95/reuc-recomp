# Laptop compatibility implementation plan

> **For agentic workers:** Use superpowers:executing-plans. User authorized implementation without further approval. Preserve existing uncommitted work; do not commit.

**Goal:** Disable unsupported DLSS, run without Vulkan maintenance1, and show actionable startup failures.

**Architecture:** Keep maintenance1 fences where supported. Otherwise reuse per-image presentation semaphores, retire resized chains after a successor image is presented and reacquired, and use the conventional device-idle shutdown fallback. Normalize saved upscaling against the completed GPU report. Show startup exceptions in a native dialog for interactive launches, preserving crash logs and unattended harness behavior.

**Tech Stack:** C++20, Vulkan 1.3, SDL3, ImGui, Win32, CMake/CTest, Python uctest.

**Spec:** User request in this chat, 2026-10-26 (no separate design approval requested).

## Global constraints

- Preserve unrelated working-tree changes; no commits or deployment to the installed game.
- Live uc.exe tests use the harness, isolated saves, 1280x720 window, muted and unlocked.
- Keep Vulkan 1.3, dynamic rendering and synchronization2 requirements.
- No extra dependencies or user settings for compatibility detection.

## Review focus

- Saved DLSS/FG on unsupported GPUs, and delayed or failed feature probes.
- Drivers with surface extensions but no device extension or feature bit.
- Repeated resize before any image of the successor has been presented/reacquired.
- Out-of-date acquisition/presentation and shutdown with outstanding work.
- Interactive startup errors versus feature probes and unattended harness failures.

### Task 1: Unsupported upscaling

Files: game/feature_report.h, launcher/main.cpp, game/settings_menu.cpp, tests/test_feature_report.cpp, launcher smoke checks as needed.

- [x] Add regression assertions for saved unsupported DLSS/FG, supported DLSS, FSR and unavailable effects.
- [x] Run targeted tests and observe RED.
- [x] Add shared option normalization; apply after a completed probe and before launch; disable unsupported selected items.
- [x] Run targeted tests and native launcher smoke (no OS input).

### Task 2: Vulkan fallback

Files: renderer/vulkan_renderer.cpp, renderer/vulkan_renderer.h, tests/renderer_smoke.cpp and test-only Vulkan capability filtering.

- [x] Add test-only extension/feature filtering to existing renderer smoke; exercise the real renderer with surface, device and feature support separately missing.
- [x] Observe startup failure with extensions hidden.
- [x] Make extension and feature enablement conditional. Keep per-image semaphores; use successor acquire completion to retire legacy chains. Guard all present-fence operations.
- [x] Run filtered and normal renderer smoke with validation, including resize, zero extent, completion and shutdown.

### Task 3: Visible startup failures

Files: game/uc_main.cpp, small startup-error helper and focused tests if needed, tests/CMakeLists.txt.

- [x] Add behavioral coverage for interactive failure message (reason/log path), probe and harness suppression.
- [x] Observe RED; implement native startup error dialog while keeping diagnostic/crash reporting.
- [x] Run targeted tests, build.bat --no-install and full CTest suite.
- [x] Run uctest train_gameplay, inspect capture, and perform final review of this task's changes.

## Execution ledger

Ruling: Work in the current checkout; the reported build depends on existing uncommitted logging changes. Snapshot pre-existing contents for review isolation.
Ruling: Execute inline; user waived further checkpoints. No commits; keep this ledger instead of commit-based task bookkeeping.
Ruling: Legacy shutdown uses device idle, the conventional limitation documented by Khronos; normal maintenance1 shutdown remains explicitly fenced.

Task 1: complete — feature-report tests RED (missing API) to GREEN; real ImGui test clicks unsupported DLSS and confirms Off persists; native launcher smoke exit 0.
Task 2: complete — filtered surface-extension smoke reproduced the laptop's exact startup error before the fix. Surface/device/feature filtering and normal maintenance1 presentation smoke all pass with validation, pixels, repeated resize, zero extent, semaphore reuse and shutdown. Test-only interception; production has no test switch.
Task 3: complete — startup message policy/content tests RED (missing helper) to GREEN. build.bat --no-install exit 0, 198/198 CTest pass, dist staged. train_gameplay PASS (7.8s), native launcher smoke PASS.
Ruling: Use focused --presentation-only GPU smoke alongside the existing full renderer smoke. The latter fails its descriptor_pool_creations assertion on both legacy and ordinary maintenance1 paths; leave that independent assertion and production allocator unchanged.
Additional check: controls FAIL: scenario expects pause=1, latest frame reports pause=2; screenshot shows valid gameplay, no crash. No controls code changed.
Environment: sandbox caused test_settings_menu 0xc0000409 and Python test startup stalls; rerun outside sandbox passed. Third-party ReShade implicit layer failed to load; GPU validation uses VK_LOADER_LAYERS_DISABLE=~implicit~ per harness convention. One ordinary presentation smoke ended during initialization without diagnostic; repeat exit 0.
Final review: one P2 finding confirmed and fixed. Log startup exceptions for unattended launches too, then reset DiagnosticLog before fatal/ExitProcess so the pipe drains. No other actionable findings in the task delta.
Final verification after the review fix: build.bat --no-install exit 0; 198/198 tests pass (77.77s). Intentional harness startup rejection (--headless) exits in 0.6s and runtime.log contains the exact reason followed by the drained-log marker. Normal train_gameplay passes again (5.7s, build/autotest/1792967664999000300). Build/dist hashes match for both executables.
