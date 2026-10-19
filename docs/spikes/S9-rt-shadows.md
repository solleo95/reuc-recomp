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

## Found after the first measurements: no game draw was model-valid

The final review's follow-up check counted the material records of the fight demo: 3.16 M records, **none** passed `light_model_valid` (vertex ambient 0). The classifier accepted only `texture x rasterised` at colour scale 1 followed by pass-through stages, while the game's draws use scale 2 or 4 and a second stage that adds `texture x rasterised`, `konst x rasterised` or multiplies by the previous result (about 90% of the records are `0x8F8AF` + `0x28F8A0`, `0x28F8AF`, `0x18F8AF` + `0x18FEA0/0x18FAE0`, `0x8F8AF` + `0x28F80F`). So the S9 timings above are the cost of tracing without the composite ever touching a pixel (the trace and denoise still ran); in the game the effect did nothing. The classifier now evaluates the TEV algebra over {zero, light-independent factor, proportional to the lit colour, unknown}: scale is free, a sum of proportional terms stays proportional, a bias, a subtraction, an interpolation, the rasterised colour twice or a light-independent term added to a proportional one is rejected. The measurements were not repeated (the composite adds one full-resolution compute pass, about 0.1 ms at 1707x1280); a repeat is part of the next phase's baseline.

A matched pair of frames from the live run (`options` command toggling the effect, 100 ms apart, dining car floor) now shows the shadows under the tables and chairs.

## Found by the owner in the first live test: characters turned black, the level did not change

With every mesh draw in the acceleration structure the zombies and Rebecca went almost black while the walls, floor and tables hardly changed. Two causes, both found with a debug tint of the composite (blue = unlit, magenta = lit but not model-valid, green/red = factor) on matched on/off frames:

1. **The level occludes the character lights.** The characters' GX lights are directional or point rigs placed without regard to the level; a ray toward them from inside a train car hits the ceiling or a wall, so the visibility is 0 for every light and, with a low ambient, the factor goes to 0. With only skinned draws (`DrawGpu::indexed`, per-vertex matrices) in the structure the same scene shows ordinary shading (live frames, zombies and leeches) and the structure shrinks as well.
2. **The level barely changes because most of it is skipped.** In the dining car the walls, ceiling and chairs are model-valid but receive factor about 1 (no occluder in front of them under their own lights), while the tables, the floor, the leeches and the monster are lit but model-invalid: about a third of all records (822 k of 3.1 M: `0x8F8AF` + `0x28F8A0` with stage 1 reading channel 1 of the rasteriser, plus `ras1` added by stage 2 in other shapes) mix a second lighting channel into the colour, which the one-channel factor cannot scale.

What this leaves: ray-traced shadows from characters onto lit static surfaces and onto each other, not level shadows. A level-wide shadow would need geometry the lights actually see (the light rigs are not in the level), the second lighting channel in the record and in the factor (its lights, ambient and material), and a way to keep walls from occluding their own room lights. That is a design question for the owner, not a tuning one.

## Carried forward

- The fixed acceleration-structure and capture cost (about 0.9 ms end to end) decides how much of the High budget the remaining effects get.
- Cut-outs (nets, grates, foliage) cast no ray-traced shadow yet.
- The denoiser's depth rejection without camera-motion compensation (phase 1 review): judged here only on a still camera and in the bot's slow movement; a fast pan was not looked at.
