# S9: ray-traced shadows, cost per preset (phase 2)

Date 2026-10-19, branch `feature/ray-tracing`, plan `docs/superpowers/plans/2026-10-19-ray-tracing-p2.md` (Task 11). Machine, protocol and windowing as in `S8-ray-tracing.md`: RTX 4080, harness, 1280x720 window, `--upscaler dlss --upscale-mode quality --render-scale 2` (internal 1707x1280), game unlocked, `--profile`, frame log `UC_FRAME_LOG`, the valid `fight3` demo replay (3 672 923 draws in every run, the same guest work as S8's runs: a fight, not a menu). Medians over the measured window (retraces 6843..8463, 3243 frames).

## Cost per preset (RT shadows and RT local shadows on the same level)

| Setting | `interval_ms` med / p95 | vs RT off | GPU `rt_as` med | GPU `rt_shadows` med (trace + denoise + composite) | GPU total RT | `busy_ms` med |
|---|---|---|---|---|---|---|
| off | 7.191 / 8.422 | – | 0 | 0 | 0 | 3.35 |
| low (¼ resolution, 1 ray, 1 local light, hard) | 8.160 / 9.324 | +0.97 | 0.448 | 0.064 | 0.51 | 3.75 |
| medium (½, 1 ray, 2 local lights, hard) | 8.259 / 9.120 | +1.07 | 0.447 | 0.155 | 0.60 | 3.74 |
| high (½, 1 ray soft, 4 local lights soft) | 8.248 / 9.348 | +1.06 | 0.448 | 0.199 | 0.65 | 3.74 |
| ultra (full, 2 rays soft, 8 local lights soft) | 8.829 / 10.255 | +1.64 | 0.447 | 0.686 | 1.13 | 3.83 |

One run per setting (the deterministic replay has a run-to-run sd of about 1%; differences below ~0.1 ms between low, medium and high are noise).

## Budget check (plan Task 11, step 2)

| Budget | Target | Measured | |
|---|---|---|---|
| High, GPU (AS + both shadow options) | ≤ 0.7 ms | 0.65 ms | met |
| High, end to end | ≤ 1.0 ms | +1.06 ms | missed by about the noise |
| Medium, end to end | ≤ 1.2 ms | +1.07 ms | met |
| Low, end to end | ≤ 0.6 ms | +0.97 ms | missed |
| Ultra, shadows alone | well inside 9.5 ms | +1.64 ms | met |

The shadow pass is cheap: 0.06 to 0.69 ms GPU at every level, and the costs rise strictly from low to ultra. The part of the cost every level pays is the acceleration structure (0.45 ms GPU) and its CPU-side capture (about 0.5 ms), together about 0.9 ms end to end, which is what S8 measured for the probe. Lowering a preset's resolution or rays cannot reduce it, so the presets stay as they are (this plan's initial table); the Low budget of 0.6 ms is not reachable until the fixed cost drops (S8 option 3: refit instead of rebuild, drop small or distant geometry, no per-packet CPU copy). The later effects (ambient occlusion, reflections, lighting) share this fixed cost; at High it already takes about a third of the 2.9 ms of all ray tracing.

## Off path (plan Task 11, step 3)

Not benchmarked a second time. With the master switch off, or both shadow levels off, the frame records no RT pass and allocates nothing (`renderer_effects_smoke --rt-shadows` section (d): `pass_runs(rt_shadows)` and the signal image count stay constant), and phase 2 adds one boolean test per frame to that path. The bench cannot resolve below about 2 to 3% on this machine (S8: +1.0%, CI −4.4% to +6.4%; phase 1 final: +3.1%, CI −7.4% to +13.7%), so a third paired run would not change the open question about the phase 1 five-attachment pipelines.

## Live look (plan Task 11, step 4)

Two harness runs of `uctest run fight --measure 8` (DLSS, windowed 1280x720): RT shadows and local shadows `high` (151 active windows, mean 118.5 FPS, minimum 80, frame gap p99 12 ms) and RT off. The bot reaches different game moments in the two runs, so their frames show different parts of the dining car and cannot be compared pixel for pixel; the deterministic smoke fixture is the reference for the shadow values (sun signal 0.377 against 0.377 computed on the CPU, point light 0.331 against 0.329). In the RT-on frames the dining car is shaded consistently, the table and chair shadows fall away from the lights, and there is no flicker, banding or blotches (three frames 100 ms apart look steady). A matched before/after capture of the same retrace is still to be taken (it needs a demo replay with frame capture at a fixed retrace).

## Carried forward

- The fixed acceleration-structure and capture cost (about 0.9 ms end to end) decides how much of the High budget the remaining effects get.
- Cut-outs (nets, grates, foliage) cast no ray-traced shadow yet.
- The denoiser's depth rejection without camera-motion compensation (phase 1 review): judged here only on a still camera and in the bot's slow movement; a fast pan was not looked at.
