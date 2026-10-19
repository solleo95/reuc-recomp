# S12: ray-traced reflections (phase 5)

Date 2026-10-20, branch `feature/ray-tracing`, plan `docs/superpowers/plans/2026-10-20-ray-tracing-p5-reflections.md`. Protocol as in S9 to S11 (RTX 4080, 1280x720 window, DLSS Quality at render scale 2, `fight3` replay of 134 013 draws per 60 retraces, 3243 frames, one run per setting).

## What was built

Mirror rays against the acceleration structure on the surfaces the game marks glossy; the hit's colour is the frame's own colour at the hit's screen position when the hit is on screen and not hidden by a nearer surface (3% depth agreement), faded toward the screen edge and with distance; a miss, an off-screen or hidden hit brings no reflection. An own denoiser (radiance) filters the signal, a composite lerps the colour toward it by `fresnel x reflectivity x 4` (`renderer/rt_reflect_math.h`, `renderer/shaders/rt_reflect.slang`, `renderer/rt_reflection_pass.cpp`). Presets (divisor, rays, cone, reach): low 4, 1, 0, 30; medium 2, 1, 0, 45; high 2, 1, 0.05, 60; ultra 1, 2, 0.08, 80.

Why the frame supplies the colour: the acceleration structure holds positions only, so a hit has no texture or lighting of its own; the frame already holds the finished picture (lit by the game, with shadows and AO) for everything the camera sees. What the camera cannot see is not reflected.

## What the game gives

Survey of the first scene (channel 1 and its specular material colour per draw): 0 for matte draws, 0.02 to 0.10 for the common wood and metal class, 1.0 for the seat, wall and floor class (a texture times the specular modulates it there, so the strength is unknown). Reflectivity = half the square root of the specular material's luminance when channel 1 is lit with the specular function.

## Result

Subtle. Live pairs (first scene, High): 0.4 to 0.6% of the pixels change by more than 8 grey levels, mostly on the luggage racks, armrests, curtain rods and characters (the glossy draws). Raising the weight 12 times and giving every up-facing surface a base reflectivity (a floor rule, tried and dropped) changed 1% to 2%: the car is dark and its surfaces reflect similar dark surfaces, the visible floor is a narrow aisle, and the windows are alpha-blended draws that are neither in the G-buffer nor in the acceleration structure. Reflections need contrast (bright lights, windows, polished floors) that this scene mostly lacks; other places of the game may show more.

## Cost (fight3, RT off 6.87 ms)

| Setting | `interval_ms` med / p95 | vs off | GPU reflection pass |
|---|---|---|---|
| low | 7.57 / 8.59 | +0.70 | 0.08 |
| medium | 7.98 / 8.78 | +1.11 | 0.20 |
| high | 8.37 / 9.98 | +1.50 | 0.24 |
| ultra | 9.17 / 10.50 | +2.30 | 0.97 |
| shadows + local shadows + AO + reflections, all High | 8.83 / 10.37 | +1.96 | 0.39 + 0.24 |

Everything at High: about 113 FPS in the heavy fight (target about 100). The acceleration structure (0.37 ms GPU) is the fixed cost every effect pays.

## Checks

`renderer_effects_smoke --rt-reflections`: a glossy floor under a red plate: the floor point whose mirror ray reaches the plate takes `fresnel x reflectivity x 4` of the plate's colour (144, 107, 71 becomes 185, 67, 45 against 185, 67, 45 computed); a matte floor, a hit hidden by a nearer quad, the effect off and the master switch off leave the floor unchanged; validation clean. `test_rt_reflect_math`: presets, reflectivity, packing in the record flags, fresnel, mirror, projection round trip, depth agreement.

## Review (fresh opus, 0 Critical, 2 Important)

* The signal is the colour DIFFERENCE (hit minus the pixel's own colour, zero where nothing is reflected), added by the composite, not an absolute radiance lerped in: with an absolute signal a miss carried one block sample of the pixel's own colour (divisor 2 or 4), which blurred the texture of every glossy surface by the weight, and the denoiser's history of a moving picture trailed it.
* Still true (deferred): the denoiser keeps up to 32 frames of a view-dependent signal and reprojects it with the surface's motion, so a moving enemy's reflection trails for a fraction of a second; the composite upsample and the à-trous pass have no material test, so a reflection fades within 1 to 4 pixels of a coplanar matte draw; the raster occlusion (GTAO, shadow map) runs after the reflections and darkens them again; the guide sample and the trace sample differ by up to a pixel when the region is not a multiple of the divisor (as in the shadow pass).

## Not covered

* Windows and other blended draws (not in the structure), per-material overrides (`dist/rt_materials.json`: no tool to pick materials yet; the first-scene floor and seats could be marked if the owner wants them to mirror), rough reflections beyond the preset cone, reflections of what is off screen (no environment map).
* The strength constant 4 and the 3% depth tolerance were set by eye on this scene.
