# S13: ray-traced lighting (phase 6)

Date 2026-10-20, branch `feature/ray-tracing`, plan `docs/superpowers/plans/2026-10-20-ray-tracing-p6-lighting.md`. Protocol as in S9 to S12 (RTX 4080, DLSS Quality at render scale 2, `fight3` replay, 3243 frames, one run per setting).

## What it is

One diffuse bounce, as a second mode of the reflection pass (`renderer/shaders/rt_reflect.slang` mode 1, one more instance and denoiser): cosine-weighted rays from each lit surface; a hit's colour is the frame's own colour at the hit's screen position when the hit is on screen and visible there; the signal is the mean of those colours, the composite adds `albedo x signal x strength` (albedo: the RT albedo target) faded by fog, clamped to 1: it only adds light. The game's ambient term is not removed (0.04 on the first scene: nothing to double count). Nothing is invented off screen. Presets (divisor, rays, reach, strength): low 4, 1, 8, 0.5; medium 2, 1, 12, 0.6; high 2, 1, 16, 0.7; ultra 1, 2, 24, 0.7 (`renderer/rt_light_math.h`). The denoiser keeps 16 frames.

## Result

Mild. First scene, High: 1.6% and 2.5% of the pixels brighter by more than 8 grey levels (mean +1.5), concentrated where bright surfaces are near dark ones: the ceiling around the lit panel and the chandelier, the tops of seats and the walls under the lamps; nothing darkens. Frame to frame on a paused scene the picture changes by 0.03 grey levels on average (p99 1.3): no shimmer at any level. The lighting is what the picture already shows redistributed: it cannot add light the camera does not see, and the game's lights are camera-relative (S10), so the bounce follows them with the picture.

## Cost (fight3, RT off 6.74 ms)

| Setting | `interval_ms` med / p95 | vs off | GPU lighting pass |
|---|---|---|---|
| low | 7.89 / 8.73 | +1.15 | 0.09 |
| medium | 8.05 / 8.90 | +1.32 | 0.25 |
| high | 8.05 / 8.87 | +1.32 | 0.30 |
| ultra | 9.43 / 10.38 | +2.70 | 1.52 |
| all five effects at High | 8.72 / 9.77 | +1.98 | 0.39 + 0.23 + 0.30 |

All five at High: about 115 FPS in the heavy fight (target about 100). Ultra (full resolution, 2 rays) is the expensive one.

## Checks

`renderer_effects_smoke --rt-lighting`: a lit floor in front of a bright red wall: 0.5 units from the wall the red channel rises from 87 to 139 against 142 computed from the cosine-weighted rays (green and blue unchanged); 2.4 units from it the rise is smaller and also matches; the effect off and the master off change nothing; validation clean. `test_rt_light_math`: presets, the additive composite, the hit weight.

## Not covered

* The effect is subtle in the dark first scene; brighter places should show more. A strength constant per preset was set by eye.
* Hits off screen or hidden from the camera bring no light, so large bright areas behind the camera do not light the scene (no environment).
* No exclusion of baked (unlit) surfaces other than the lit-surface gate; characters get the bounce like the level.
