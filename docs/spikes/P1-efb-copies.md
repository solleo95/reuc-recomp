# P1: EFB texture copies (2026-10-31)

Counter added to the Vulkan renderer (`VulkanRenderer::take_copy_stats`, logged once a second as `[efb-copies]` with `--profile`). One Steam Deck run, first scene, owner's settings, steady state:

| per second | value |
|---|---|
| EFB texture copies | 1132-1168 (about 16 per presented frame at 70 FPS) |
| copies covering at least 90% of the 640x480 EFB | 0 |
| copies that clear the EFB afterwards | 0 |
| EFB areas copied (sum of copy areas / 640x480) | 343-377, i.e. about 5 EFB areas per frame, 0.3 per copy |
| GPU time of the copies (`[gpu-sum]` texture copies) | 63-68 ms/s, about 57 us per copy |
| GPU time of EFB draws (`efb draws`) | 83-96 ms/s |

## Reading

- The copies are many and small: the cost per copy is mostly fixed (one blit, the layout barrier to shader-read, mip blits and barriers when the game asked for a mip chain), not pixels.
- Nothing here is a full-frame copy that could be dropped wholesale. The saving, if any, is per-copy overhead: merging the final `vkCmdPipelineBarrier2` of consecutive copies into one, or skipping copies whose target is never sampled before the next clear. The second needs a "sampled" flag on the copy target set where textures are bound (all three backends); it was not added.
- Windows figures are missing: `fight4` replay needs the exe beside the game data (the game folder), which carries the deployed build without this counter. They come with the final deploy.

## Candidates (owner decision, nothing is removed)

1. One barrier for a run of consecutive copies in a packet (exact, Vulkan only; D3D12/D3D11 transition on demand already).
2. Skip a copy whose target is never sampled (needs usage tracking; exactness is by construction, the risk is the tracking).
3. Look at `efb draws` (83-96 ms/s) separately: not part of this investigation.

## Continuation baseline (2026-10-31)

The latest Claude session completed the roadmap and the nine deferred review fixes; no experiment was left running. The current Deck binary is 72,497,856 bytes. No product code was changed for this baseline.

One `py -3.13 scripts/linux/deck_perf.py --owner --seconds 10 uc` run passed, after checking that no `uc` process was running. Artifacts: `build/autotest/1793420548302239800/`. The harness used its 1280x720 window and isolated saves; the capture shows train gameplay. The game exited and no test files remained beside the staged binary.

The script's ten-record summary includes the x8-to-x1 transition (68.5 FPS, worst frame 375 ms). Excluding the transition record and the following record whose frame-time window still contains that transition leaves eight steady records: 71.48 FPS, lowest/highest second 63.85/76.92, mean of per-second p99 values 16.08 ms, worst frame 21.08 ms. On those same records, EFB draws average 94.2 ms/s and texture copies 71.2 ms/s. This is a live baseline, not evidence of an optimisation gain.

Source candidate: `VulkanRenderer::record_texture_copy` transitions the last mip from transfer destination to transfer source, then transitions every mip to shader-read. The last mip has no subsequent blit reader; with one mip, the whole intermediate transfer-source transition exists only to give the final barrier a common old layout. A follow-up could combine the final transitions, preserving transfer-source layouts for earlier mips and transfer-destination for the last mip. The scene-restore caller also uses this helper, so both callers and mipmapped copies need exact-output coverage. Savings and effect on FPS are unmeasured; implementation awaits the next direction decision.

## Final-mip barrier experiment (2026-10-31)

Owner authorised autonomous continuation. The final transfer-source transition is removed; one dependency command now finishes the final destination mip and the earlier source mips in their actual layouts. This saves one `vkCmdPipelineBarrier2` per EFB copy and leaves every blit and inter-mip dependency intact. Vulkan only; D3D paths are unchanged.

The existing GPU smoke now forwards intercepted Vulkan commands to the real driver and asserts that a three-mip copy puts only two mips in source layout, and a one-mip copy puts none. RED: old code fails the three-mip assertion. GREEN: both assertions and the smoke's sampled-copy pixels pass; lighting golden hashes, motion blur, and restored-copy/RCAS checks also pass. Full Windows build and Linux cross-build pass; CTest 231/231 pass. One upscale-smoke failure was intermittent at `sl.common.dll_unloaded+0x3e490` during another renderer's initialization; baseline and candidate repeats passed. Vulkan validation stops at the previously recorded unused shader-colour-output warning; no synchronization error was reported before that stop.

Short deterministic `opening` replay, five game seconds measured, unlocked. Each run starts from the same isolated saves. The shorter recording respects the owner's preference against long fight runs.

| Platform / effects | Before wall seconds | After wall seconds | Work per run |
|---|---:|---:|---:|
| Deck / owner, order before then after | 9.918 | 10.054 | 443158 draws |
| Deck / owner, order after then before | 10.019 | 10.003 | 443158 draws |
| Deck / defaults, order after then before | 9.980 | 9.994 | 443158 draws |
| Windows Vulkan / owner | 2.918 | 2.927 | 445470 draws |

Within each pair, retraces and draws match. The wall-time changes are within noise; no resolved FPS gain is demonstrated. Platform draw counts differ and are compared only within each platform's pair.

Live first scene, one run per variant/configuration, ten-second measurement with the first two log records excluded to remove the x8-to-x1 transition and its overlapping frame-time window:

| Settings / binary | Steady mean FPS | Max / lowest second | Mean per-second p99 / worst frame, ms | Copy GPU us/copy |
|---|---:|---:|---:|---:|
| owner / before | 69.19 | 77.53 / 61.75 | 17.03 / 20.14 | 62.42 |
| owner / after | 70.47 | 71.95 / 67.00 | 15.94 / 17.65 | 61.17 |
| defaults / before | 78.78 | 82.99 / 71.95 | 16.13 / 27.51 | 64.30 |
| defaults / after | 78.27 | 82.74 / 69.00 | 15.49 / 17.22 | 64.45 |

Run directories, in table order: `build/autotest/1793421114170507300`, `1793421164266064600`, `1793421214042539000`, `1793421263467344600`. Owner runs use per-pixel lighting, ultra blur, bloom 150 and the explicitly selected 90 FPS limit; default runs are unlocked. Captures were inspected. The live differences are too small to claim a speedup or regression. Kept under the owner's established rule to keep exact small reductions in work. Heavy-fight performance and a bounded statistical improvement remain unproven.

Final independent review: no actionable findings. Final `build.bat` passed its nine pending tests and installed Windows; the prior full CTest passed 231/231 in 161.22 seconds. Linux was staged with `scripts/linux/deck.py stage` and the installed Deck binary's SHA256 equals the tested cross-build (`85b930f67aed10b7b4f34fc9c4b72b99fb528a834658fb2a917f0cee46b7bac5`). Temporary before/after binaries, the Deck benchmark directory and launch override were removed. Deck free space: 43 GB. Changes remain uncommitted.
