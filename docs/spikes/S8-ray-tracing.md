# S8: ray tracing cost on a real fight frame

Date 2026-10-19, branch `feature/ray-tracing` (HEAD a64816d: phase 0-1 tasks 1-2, device support, per-frame acceleration structures, `UC_RT_PROBE=1` debug probe). The probe was removed in phase 2 (plan p2, Task 12) once the ray-traced shadow pass replaced it; the numbers below stay as measured with it.
Machine: RTX 4080, harness `uctest`, game unlocked (`--fps-limit 0`), guest pinned to CPU 26, implicit Vulkan layers off, `--profile`, per-frame CSV via `UC_FRAME_LOG`.

This document replaces an earlier version whose numbers came from a stale `fight2` demo (recorded before the 120 Hz pacing commit 67c5eb6, so it replayed the loadout menu) and from a desktop-sized window. Everything below is measured on the re-recorded `fight2` / the valid `fight3` / the live bot, with a 1280x720 window.

## What was measured

- Window 1280x720, windowed (harness default; no `--width/--height/--window-mode` override).
- `--render-scale 2` (720p multiplier 2) makes the EFB 2560x1920 (visible 2560x1440). `--upscaler dlss --upscale-mode quality` (ratio 1.5) gives the DLSS scene (internal) 1707x1280 -> DLSS output 2560x1920 EFB, which the window then presents at 1280x720. So the GPU work equals the owner's reference: 1440p output, DLSS Quality, internal 1707x1280 (the game's EFB is 4:3; the 16:9 visible part is 1707x960 of it). Game log: `Native render: 2560x1920 EFB, 720p multiplier 2x ... output 1280x720` and `Upscaling: scene 1707x1280 -> 2560x1920 EFB (DLSS)`.
- The probe measures one shadow ray per pixel toward the dominant directional light at full internal resolution, plus the acceleration structure (AS) builds. It is a lower bound for the per-effect ray cost (a real effect adds denoising, more rays per pixel for AO/reflections, and per-material hit shading).
- `gpu_ms` is the longest single GPU submission, not a whole-frame GPU time; the end-to-end `interval_ms` (present to present) is the number that decides FPS. The RT-off frame is mostly guest/CPU bound (render thread busy about 3.5 ms, GPU timed passes 0.8 ms).
- Deterministic demos run on a virtual clock: their `interval_ms` is wall time per presented frame for fixed guest work, which is the right frame-cost figure, but live FPS confirmation of them was not done (the two live bot runs below are a rough cross-check only).

## Commands

```
cd scripts
BASE="--game-arg=--upscaler --game-arg=dlss --game-arg=--upscale-mode --game-arg=quality --game-arg=--render-scale --game-arg=2 --game-arg=--profile"
# 1 live bot, RT off                       (run build/autotest/1792390200865470900)
UC_FRAME_LOG=off.csv                     py -3.13 -m uctest run fight --measure 20 $BASE
# 2 live bot, probe                        (run build/autotest/1792390335723697500)
UC_RT_PROBE=1 UC_FRAME_LOG=probe.csv     py -3.13 -m uctest run fight --measure 20 $BASE
# 3 fight3 demo, RT off and probe          (build/autotest/demo-1792390404156 / demo-1792390446868)
X="dist/uc.exe|--upscaler|dlss|--upscale-mode|quality|--render-scale|2"
UC_FRAME_LOG=f3_off.csv                  py -3.13 -m uctest bench "$X" --demo fight3 --runs 1
UC_RT_PROBE=1 UC_FRAME_LOG=f3_probe.csv  py -3.13 -m uctest bench "$X" --demo fight3 --runs 1
# 4 off-path bench on the re-recorded fight2 (build/autotest/demo-1792390539300 .. demo-1792390721625)
py -3.13 -m uctest bench dist/uc_before.exe dist/uc.exe --demo fight2 --runs 3
```

Statistics are taken over the frame-log rows inside the measured window (retraces between the slowdown point / demo window start and its end). `dist/uc_before.exe` is `main` (f841460) built in a separate git worktree (removed again) with the same CMake flags, the same `project/pgo` profile and the same `build/_deps`; `dist/uc.exe` is HEAD (a64816d) built the same way. `uc_before.exe` contains no `UC_RT_PROBE` / `gpu_rt_as_ms` strings, `uc.exe` does.

## Validity of every run (combat, not a menu)

| Run | Check | Result |
|---|---|---|
| 1 live bot, RT off | capture `frame-1792390222323710-50104-50256.png` taken at the slowdown point; `fps50.csv` state per 50 ms window | dining car, leeches, a zombie under the crosshair, HUD "Ребекка": combat. State `active` for the first 169 windows (8.45 s); after that the player is `game_over` (screen of the fixed 480 FPS menu, 2 ms frames). Only the active 8.44 s are used (1131 frames, 134 FPS, harness mean 135.0). |
| 2 live bot, probe | capture `frame-1792390360554787-42068-49276.png`; `fps50.csv` | dining car, leeches, a zombie: combat. 390 windows, 0 non-active, so the full 20 s are gameplay. |
| 3a fight3 RT off | `[detbench]` draws per 60 retraces in the window 6843..8463 | 131791-151220, settling at ~133-135k: leech wave (a menu is a flat ~66000). |
| 3b fight3 probe | same | identical sequence to 3a draw for draw (total 3 672 923 draws in both): same guest work, so the pair is a valid comparison. |
| 4 fight2 off-path bench (6 runs) | `[detbench]` draws per 60 retraces in 7945..9145 | 90685 (first second, entering the fight) then up to 158363; 2 980 407 draws in every run (~149k/s). Not the menu (flat ~66000 / `mesh 0 calls`). |

No run had to be discarded.

## Measurements

Median / mean / p95 over the measured window, per presented frame.

| Run | frames | `interval_ms` med / mean / p95 | FPS (from mean) | render-thread `busy_ms` med | `gpu_ms` med / p95 | `gpu_rt_as_ms` med / p95 | `gpu_rt_probe_ms` med / p95 | `rt_blas` | `rt_triangles` med / max |
|---|---|---|---|---|---|---|---|---|---|
| fight3 demo, RT off (heavy leech wave, 23.6 s wall for 27 game s) | 3243 | 7.114 / 7.278 / 8.456 | 137.4 | 3.47 | 0.80 / 1.26 | 0 | 0 | 0 | 0 |
| fight3 demo, `UC_RT_PROBE=1` | 3243 | 9.234 / 9.326 / 10.416 | 107.2 | 4.14 | 1.87 / 2.44 | 0.447 / 0.710 | 1.016 / 1.455 | 1 | 41 976 / 50 250 |
| live bot, RT off (8.4 s active) | 1131 | 7.351 / 7.473 / 8.245 | 133.8 | 3.71 | 0.80 / 1.31 | 0 | 0 | 0 | 0 |
| live bot, probe (first 8.45 s of the window) | 826 | 10.130 / 10.241 / 11.616 | 97.6 | 4.53 | 2.01 / 2.83 | 0.448 / 0.826 | 1.069 / 1.800 | 1 | 45 272 / 46 232 |
| live bot, probe (full 20.1 s) | 1976 | 10.048 / 10.167 / 11.580 | 98.4 | 4.49 | 2.06 / 2.86 | 0.455 / 0.867 | 1.125 / 1.848 | 1 | 45 684 / 46 232 |

Reading it:

- The paired comparison is the fight3 demo (identical guest work and draws): the probe turns 7.11 ms into 9.23 ms, +2.12 ms median (+2.05 ms mean, +1.96 ms at p95). Of that, GPU-timed RT passes are 0.45 (AS) + 1.02 (probe) = 1.46 ms; the rest (~0.6 ms) is CPU side: render-thread busy +0.67 ms, guest cycles per frame 17.8 -> 21.2 M (instance capture, transform dispatch, AS build recording, synchronisation).
- The two live bot runs are not a pair: the bot reached the slowdown point at different game seconds (140 vs 168) and different scene weight. They only cross-check the demo: RT-off 134 FPS live vs 137 FPS in the demo; probe 98-99 FPS live vs 107 FPS in the demo (probe live scene heavier). AS 0.45 ms and probe ~1.0-1.1 ms agree across all three probe runs.
- One BLAS (all draws merged), ~42-46k triangles typical, up to ~50k.
- One live RT-off hitch of 75 ms and a 313 M-cycle frame occur in the live runs (RT off included): not RT related.
- Compared with the earlier (invalid-setup) S8, the probe is ~0.4 ms more expensive (1.0 vs 0.63 ms) because the internal target is the true 1707x1280 EFB, 33% more pixels than the 1707x960 the earlier run counted.

### Off path (step 3, re-recorded fight2, RT off)

```
round 1: before 14.80 s, uc 15.30 s   round 2: uc 16.08 s, before 15.97 s   round 3: before 15.50 s, uc 15.34 s
dist/uc.exe vs dist/uc_before.exe: +1.0% wall time, 95% CI [-4.4%, +6.4%] over 3 paired runs -> INCONCLUSIVE; best vs best +3.3%; peak FPS (1 s) 173.0 -> 166.4 (-3.8%)
guest Mcycles per run: before 60027 / 64602 / 62413 (mean 62347), uc 61522 / 64439 / 61600 (mean 62520, +0.3%)
```

The expected result (within 1%) is not demonstrated: the bench noise on this machine today is 3-4% between runs (sd 3.8% before, 2.8% uc), larger than usual, so a 1% effect cannot be resolved with 3 rounds. What the data does say: wall time +1.0% (mean), guest CPU cycles +0.3%, identical guest work; best-vs-best +3.3% and peak FPS -3.8% (uc lower in rounds 1 and 3) lean against `uc.exe`, so a small off-path cost (RT usage flags on mesh buffers, per-packet bookkeeping) is neither excluded nor shown. Not repeated (one bench per question); re-check after Task 4 with `--runs 3` on a quiet machine, or by A/B with `UC_RT_PROBE` unset on both exes.

## DLSS Ray Reconstruction check (step 4)

| Item | Result |
|---|---|
| `sl.dlss_d` supported on the 4080 | Yes. `dist/uc.exe --features` today: `"ray_reconstruction":true,"ray_reconstruction_reason":""`, `"ray_tracing":true`. `sl.dlss_d.dll` (signature verified) and `nvngx_dlssd.dll` are in `dist` next to `nvngx_dlss.dll`. |
| Licence for `nvngx_dlssd.dll` | The Streamline package (`build/_deps/streamline-src/bin/x64`) has only `nvngx_dlss.license.txt`, `nis.license.txt`, `reflex.license.txt`; none is specific to `nvngx_dlssd.dll`, and `docs/ProgrammingGuideDLSS_RR.md` has no licence or redistribution text. `nvngx_dlssd.dll` ships in the same folder as `nvngx_dlss.dll`, so "NVIDIA RTX SDKs LICENSE" (`nvngx_dlss.license.txt`, also installed as `dist/licenses/Streamline/nvngx_dlss.license.txt`) is the governing text. Its first section defines the SDK as the NVIDIA RTX SDKs "including the DLSS SDK, NGX SDK, ..." and grants (1c) distribution of SDK software in object code inside an application, subject to 2a (material additional functionality beyond the SDK), 2c (distribute under terms at least as protective), 2d (report known non-compliant distribution) and 4b (no stand-alone distribution). That permits shipping the file next to `nvngx_dlss.dll`. This is an inference from the shared package and licence, not an explicit statement about `nvngx_dlssd.dll`; confirm with NVIDIA (nvidia-rtx-license-questions@nvidia.com, named in the licence) if certainty is wanted. `THIRD_PARTY_NOTICES.md` lists only `nvngx_dlss.dll` / `nvngx_dlssg.dll`: add `nvngx_dlssd.dll` before release. |
| Jitter and projection inputs (`renderer/dlss_upscaler.cpp`, lines 54-82) | Already passed to `slSetConstants`: `cameraViewToClip` (+ inverse), `clipToPrevClip` (+ inverse), `jitterOffset`, `mvecScale` (internal pixels), near/far/FOV/aspect derived from the projection, `depthInverted=false`, `cameraMotionIncluded=true`, `motionVectors3D=false`, `orthographicProjection=false`, `motionVectorsDilated/Jittered=false`, `reset`. DLSS-RR uses the same `slSetConstants`, so this plumbing is reusable. |
| What RR needs that does not exist yet | `slDLSSDSetOptions` (`worldToCameraView`/`cameraViewToWorld`, normal-roughness mode) and the guide buffers: albedo, specular albedo, normal+roughness, specular hit distance, plus depth, motion and the noisy colour. These come from the phase 1-4 G-buffer and RT outputs. |

Verdict: no failure found; RR stays available as an option and is decided by a quality test in phase 4. If it is dropped, the own denoiser runs everywhere (spec).

## Decision (step 5)

Remaining High budget = 1000/120 - RT-off frame:

| RT-off frame | value | remaining High budget |
|---|---|---|
| fight3 demo (paired with the probe pair, heavy leech wave), median | 7.114 ms | **1.22 ms** |
| fight3 demo, mean | 7.278 ms | 1.06 ms |
| live bot, median (8.4 s window) | 7.351 ms | 0.98 ms |
| fight3 demo, p95 | 8.456 ms | -0.12 ms |

Rule: GO when the remaining High budget >= 2 ms. 1.22 ms (and 0.98 ms from the live run) is below it, so the result is **NO-GO**; per the plan, work stops here for the owner's decision before Task 4.

Why this is not a noise-level miss: the probe alone (AS 0.45 ms + one full-resolution shadow ray 1.0 ms + ~0.6 ms CPU-side) costs +2.1 ms end to end, i.e. 1.7 times the whole High budget, for one effect. The spec's High split (AS 0.5, shadows 0.4, local shadows 0.6, AO 0.4, reflections 0.7, lighting 0.7, denoise 0.2 = 3.5 ms) needs about 3 times the available 1.22 ms. Ultra stays feasible: 1000/60 - 7.11 = 9.55 ms against the spec's 10 ms Ultra budget (tight, it needs the Ultra split rechecked).

### Re-split from the measured unit costs (what 1.22 ms would have to look like)

Unit costs: AS 0.45 ms (p95 0.71-0.87), one full-res (1707x1280) ray pass about 1.0 ms, CPU-side overhead about 0.6 ms for the probe path (some of it per frame setup shared by all effects).

| Item | Spec High (ms) | Fits in 1.22 ms? |
|---|---|---|
| AS build + capture | 0.5 | 0.45 measured; leaves 0.77 ms for everything else |
| Five effects + denoise | 3.0 | would need to average ~0.15 ms each, i.e. ~15% of a full-resolution ray pass each (eighth-resolution rays or fewer effects), plus the CPU overhead |

### Options for the owner (work is stopped until one is chosen)

1. Lower High presets: High keeps at most one or two RT effects (for example shadows from the dominant light plus one other) at half or quarter resolution; the rest move to Ultra only. Estimated: AS 0.45 + 2 effects at quarter-res ~0.25 each = ~0.95 ms plus CPU overhead, borderline at 1.22 ms.
2. Lower the High FPS target. Remaining budget by target, from this 7.11 ms RT-off frame: 100 FPS -> 2.89 ms, 90 FPS -> 4.0 ms, 90 FPS fits the spec's 3.5 ms split. Ultra stays at 60 FPS.
3. Cut the fixed costs first: refit/update the BLAS instead of rebuilding every frame, drop distant or small geometry from the AS, and remove the CPU-side copy per packet; each saves part of the 0.45 + 0.6 ms before any effect is added. This is worth doing for options 1 and 2 as well, but is a Task 4 style investigation, not a measurement.

All three figures are single runs on a machine whose run-to-run noise is 3-4% (about 0.25 ms of the 7.1 ms frame); the conclusion (NO-GO by about 0.8 ms) is larger than that noise.
