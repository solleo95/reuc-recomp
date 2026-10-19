# S10: ray-traced level shadows (phase 3)

Date 2026-10-19, branch `feature/ray-tracing`, plan `docs/superpowers/plans/2026-10-19-ray-tracing-p3-level-shadows.md` (Task 18), design `docs/superpowers/specs/2026-10-19-rt-level-shadows-design.md`. Protocol as in S9: RTX 4080, harness, 1280x720 window, `--upscaler dlss --upscale-mode quality --render-scale 2` (internal 1707x1280), game unlocked, `--profile`, `UC_FRAME_LOG`, the `fight3` demo replay (combat confirmed: 134 013 draws per 60 retraces, a menu would show about 66k), medians over retraces 6843..8463 (3243 frames), one run per setting.

## What changed against S9

* Light classes: lamps (point lights of the level, `rt_local_shadows`), rig lights (weak directional fill that follows a character, `rt_shadows`), specular lights (never traced).
* Occluder classes by instance mask: static level (mask 1) and skinned characters (mask 2); a lamp's ray sees both, a rig light's only the skinned one.
* The colour model accepts channel-1 specular (`d + a` shapes), lamp rays stop `kRtLampClearance` = 1.0 view unit before the lamp.
* Static meshes use cached object-space BLASes with per-instance matrices (Task 17).

## Cost per preset (RT shadows and RT local shadows on the same level)

| Setting | `interval_ms` med / p95 | vs RT off | GPU `rt_as` | GPU `rt_shadows` | per-frame BLAS (skinned) / triangles |
|---|---|---|---|---|---|
| off | 7.332 / 8.589 | – | 0 | 0 | 0 / 0 |
| low | 8.134 / 9.512 | +0.80 | 0.370 | 0.114 | 1 / 8 541 |
| medium | 8.452 / 10.128 | +1.12 | 0.371 | 0.293 | 1 / 8 541 |
| high | 8.916 / 10.340 | +1.58 | 0.373 | 0.407 | 1 / 8 541 |
| ultra | 10.341 / 11.673 | +3.01 | 0.374 | 1.679 | 1 / 8 541 |

High runs at about 112 FPS in the heavy fight (target about 100), Ultra at about 97 FPS (target at least 60).

Against S9 (same demo, RT off 7.19 ms then, 7.33 ms now: the machine drifts by about 2%): the acceleration-structure GPU cost fell from 0.45 to 0.37 ms because the level's triangles are no longer transformed and rebuilt each frame (the per-frame BLAS holds the 8.5k skinned triangles only), but the end-to-end cost at High rose from +1.06 to +1.58 ms. What is left and why: S9's numbers were taken while no draw was model-valid, so its composite did nothing; now about 90% of the records are valid, the composite and the denoiser work on real pixels (about 0.1 ms), the lamp lights trace against the whole level (the shadow pass at High 0.20 to 0.41 ms, at Ultra 0.69 to 1.68 ms), and the CPU side (capture of the draws, instance lists) still costs about 0.5 ms. The plan's "at most the S9 number" is missed by about 0.5 ms at High; the budget that counts (High about 100 FPS, 2.9 ms for all ray tracing) is met with 0.78 ms GPU and 1.58 ms end to end.

## Live pair (first scene, High)

Matched frames of the same paused moment (the host pause key, then `options` toggles `ray_tracing`): off, on, on 100 ms later, off again. The two off frames are identical, the two on frames differ by 0.0014 grey levels on average (steady). The difference map (RT on minus off) shows: the chandelier's arms and glass shades casting their shadows on the ceiling panel around the lamp (strong, the lamp is below the panel), darkening of the characters where the other zombie and their own limbs block the rig light, and no change elsewhere: seat backs, walls and floor stay as they were because nothing blocks the lamps' rays to them in this scene (the lamps hang in the open aisle). 4.9% of the pixels become darker by more than 8 grey levels, none brighter. Characters stay normally lit (no black faces or bodies), the lamp fixtures do not black out their surroundings.

## Not covered

* Corridor and further scenes were not captured (few live runs by the owner's rule); the dining car scene of S9's live look and the first scene above are the visual evidence.
* Alpha cut-outs (curtains, grilles) cast no ray, the denoiser has no camera-motion compensation in its depth rejection (a moving camera blurs the shadow edge by a frame), the RT-off cost of the five-attachment pipelines is still unproven (bench CI ±7%).
* Clearance: 1.0 view unit was enough for the chandelier of the first scene (no black ring around the lamp in the pair); other fixtures' extents were not measured.
