# S11: ray-traced ambient occlusion (phase 4)

Date 2026-10-20, branch `feature/ray-tracing`, plan `docs/superpowers/plans/2026-10-20-ray-tracing-p4-ao.md`. Protocol as in S9/S10: RTX 4080, harness, 1280x720 window, DLSS Quality at render scale 2 (internal 1707x1280), `fight3` demo replay (134 013 draws per 60 retraces: combat), medians over 3243 frames, `UC_FRAME_LOG`.

## What it is

Cosine-weighted hemisphere rays of a limited length per pixel; the share that hits something lowers the colour by the preset's strength. It is folded into the visibility pass: one signal (`shadow factor x AO factor`), one denoiser, one composite. The composite applies to every depth-writing surface with a normal that is not a cut-out (no light model: the design's "ambient share" would do nothing here, the game's ambient is 0.04 on the first scene's seats). GTAO is replaced while it is on.

Presets (`renderer/rt_ao_math.h`; divisor, rays, radius in view units, strength): low 4, 1, 1.5, 0.7; medium 2, 1, 2.5, 0.75; high 2, 2, 3.5, 0.8; ultra 1, 2, 5.0, 0.8.

## Scale

GTAO's radius 0.5 shows almost nothing in this game: the car is about 140 view units long, a seat about 4 tall. Live tests at High with radii 1.5, 3, 5 (strength 0.8): 2.7%, 5.8% and 11.2% of the pixels darker by more than 8 grey levels; 3 to 5 reads as occlusion under the luggage racks, between seats, under the ceiling beams and around characters, without a halo. The presets scale from 1.5 to 5.

## Cost (fight3, one run per setting; RT off 6.92 ms)

| Setting | `interval_ms` med / p95 | vs off | GPU AS | GPU visibility pass |
|---|---|---|---|---|
| GTAO high (before) | 7.26 / 8.31 | +0.34 | – | 0.26 (occlusion pass) |
| RT AO low (¼ res, 1 ray) | 8.00 / 8.71 | +1.08 | 0.37 | 0.12 |
| RT AO high (½ res, 2 rays), final radius | 8.16 / 9.18 | +1.24 | 0.36 | 0.37 |
| RT AO ultra (full res, 2 rays; measured at the first radius 0.8, not repeated at 5.0) | 8.90 / 10.00 | +1.98 | 0.37 | 1.24 |
| RT shadows high + local high + AO high | 8.11 / 9.21 | +1.19 | 0.37 | 0.39 |

The first draft of the presets (High at full resolution with 2 rays, Ultra with 4 rays) cost 1.25 and 1.77 ms in the pass; half resolution for High costs a third. With shadows and AO both at High the three share one trace (same divisor): the AO adds about 0.1 ms to the shadows' 0.29. The acceleration structure (0.37 ms GPU, about 0.5 ms CPU) is still the fixed cost every RT effect pays (+1.1 ms end to end), so RT AO alone costs more than GTAO but the third effect at High is nearly free. All three at High: about 123 FPS in the heavy fight (target about 100).

## Checks

* `renderer_effects_smoke --rt-shadows` (b6): a floor beside a wall; the occlusion at 0.15, 0.35 and 0.60 units from the wall matches the CPU estimate of the occluded share of the cosine hemisphere within 0.01; the open floor stays at 0.985 or above (the denoiser's history forgets the wall after about 100 frames); GTAO does not run while RT AO is on and runs again without it.
* `test_rt_ao_math`: preset table, direction unit length and hemisphere, mean cosine 2/3, factor endpoints. `test_rt_ui`: GTAO is greyed only while RT AO is active.
* Validation clean.

## Not covered

* The AO also darkens characters and baked surfaces that are lit by the game's own shading (it is a multiplier on the finished colour, like GTAO); strength 0.8 and the limited radius keep it subtle; no per-material exclusion.
* Binary any-hit rays with no hit-distance falloff: with one ray per pixel at Low the denoiser does the smoothing (a longer history lag when something moves).
* The in-game menu's new combo was not looked at on screen; the launcher and the menu build and the host tests pass.
