# S3 — DLSS and FSR frame generation on Vulkan

Date: 2026-10-12. Method: shallow clones of the official repos, read the programming guides (no code run yet).
Sources: `NVIDIAGameWorks/Streamline` (HEAD "Streamline SDK 2.14.1 Release", 2026-09-08), `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK` (AMD FSR SDK 2.3.0 "Redstone").

## DLSS (Streamline 2.14.1)

| Question | Answer (doc reference) |
|---|---|
| Does DLSS-G (frame generation) support Vulkan? | **Yes.** `ProgrammingGuideDLSS_G.md` §16 describes Vulkan: Streamline intercepts `vkQueuePresentKHR` and `vkAcquireNextImageKHR`; the app passes device info via `slSetVulkanInfo`; `FeatureRequirementFlags::eVulkanSupported` exists |
| Synchronisation rule | Host must signal the binary `present` semaphore in `VkPresentInfoKHR` with its final submit, and wait on the binary `acquire` semaphore from `vkAcquireNextImageKHR` before starting a frame (§16.0) |
| Version floor | The guide's Vulkan 1.1 (1.3 recommended) and driver ≥ 527.64 apply only to running optical flow natively; optical flow runs in interop mode by default, so this is not a DLSS-G floor |
| Fixed multi frame generation (`numFramesToGenerate`) | Documented generically (§6.2); the docs do not restrict it to D3D12. Not confirmed on Vulkan until tried |
| **Limitations on Vulkan** | **Dynamic MFG is D3D12-only** (§6.3). **VSync together with frame generation is D3D12-only** (§22.5) |
| Reflex / PCL on Vulkan | Same `slSetVulkanInfo` path (`ProgrammingGuideReflex.md`); required Vulkan extensions are reported by `slGetFeatureRequirements` at runtime. Reflex is mandatory with DLSS-G |
| Other features | DLSS SR, DLSS-RR, DeepDVC, NIS guides present; Ray Reconstruction listed (relevant for sub-project 7) |

Verdict (DLSS): **PASS on documentation only.** The spec's criterion also asks for a minimal sample running on the RTX 4080; no sample
was built or run, so DLSS-G on Vulkan is documented, not demonstrated. Plan 6 must start with that sample. Consequences for the design:
1. Do not plan on Dynamic MFG. Fixed multi frame generation is documented via `numFramesToGenerate` and `numFramesToGenerateMax`. Which
   multipliers the RTX 4080 supports is not stated in the guide (commonly 2x on RTX 40 and up to 4x on RTX 50, uncited here); read
   `DLSSGState::numFramesToGenerateMax` at runtime and offer only what it reports.
2. Present with VSync off (or VRR) when frame generation is on. The frame limiter must be ours.
3. Binary semaphore handshake around acquire/present is part of the renderer's frame loop design from day one.

## FSR (AMD FSR SDK 2.3.0)

- `Kits/FidelityFX/readme.md` says the SDK serves "DirectX 12 or Vulkan", but its Known Issues table states: **"Vulkan is currently not supported in AMD FSR SDK 2.3"** for all effects.
- FSR Frame Generation Swapchain is a replacement DXGI swapchain, so it is DX12-only by construction.
- ML upscaling (FSR 4) additionally needs RX 9000 hardware; on other GPUs the API falls back to FSR 3.1.x.

Verdict (FSR): **FAIL for SDK 2.3 on Vulkan.** Fallbacks, to be verified in Plan 6:
1. Use the FidelityFX SDK 1.x line (FSR 3.1 upscaler) which shipped a Vulkan backend. Needs a check that the version still builds and runs with a current Vulkan SDK.
2. FSR frame generation on Vulkan is unconfirmed. Interim alternative for non-NVIDIA users: driver-level frame generation (AMD AFMF / NVIDIA Smooth Motion at the driver), or our own optical-flow interpolation pass later.
3. FSR remains the secondary path; DLSS drives the design (matches the user's priority).

## Spec impact

- Spec §8: `FsrFfx` needs a version note (SDK 1.x for Vulkan) and DLSS-G on Vulkan needs the limits above. Added in the spec's Results section.
- Reflex is a hard dependency of DLSS-G, so the late-sampled cursor design (spec §8) stands.

## Not verified

- The FidelityFX SDK 1.x Vulkan status (backend, frame generation) was not checked: the shallow clone of `main` is 2.3.0 and does not show the 1.x tags. Plan 6 must fetch the 1.x tag and read its docs.
- The exact Vulkan device extensions DLSS-G needs are reported by `slGetFeatureRequirements` at runtime and were not listed here.
- No sample was built or run. Real behaviour of fixed MFG on Vulkan, the exact extension list and the driver version needed on the RTX 4080 (driver 617.14) are open until Plan 6.
