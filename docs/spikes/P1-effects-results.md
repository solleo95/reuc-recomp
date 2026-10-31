# P1: effects on the GPU, results (2026-10-31)

Plan: `docs/superpowers/plans/2026-10-31-performance-roadmap.md`. Spec: `docs/superpowers/specs/2026-10-30-performance-roadmap-design.md`.

## Tasks 3-5 (smoke.slang): one light loop per channel, no duplicate vertex lighting, loop over set mask bits

Exactness: `renderer_effects_smoke --ppl-exact` (12 captures: six lighting cases, per-vertex and per-pixel) keeps the hashes recorded before the change; `--ppl --blur` and the D3D11/D3D12 smokes pass.

Steam Deck, first scene, 10 s, shaders swapped between runs with the same `uc` binary (byte-identical to the deployed one), alternating, 2 runs each. Window includes the x8 to x1 switch second, so the worst-frame figures are not the game.

| Settings | Shaders | FPS mean (max) | lowest second | GPU ms/s: scene / efb / copies / bloom / blur | GPU total | render thread |
|---|---|---|---|---|---|---|
| owner's | base | 67.9 (77.0) | 48 | 360 / 96 / 67 / 34 / 1.31 ms per frame | 652 | 437 |
| owner's | tasks 3-5 | 65.0 (70.9) | 44 | 347 / 92 / 65 / 34 / 1.42 | 636 | 437 |
| owner's | base | 67.1 (72.9) | 57 | 353 / 83 / 63 / 34 / 1.34 | 629 | 424 |
| owner's | tasks 3-5 | 67.1 (72.0) | 53 | 351 / 84 / 63 / 34 / 1.31 | 624 | 425 |
| defaults | base | 74.4 (83) | 42 | | | |
| defaults | tasks 3-5 | 74.8 (85) | 41 | | | |
| defaults | base | 75.8 (83) | 53 | | | |
| defaults | tasks 3-5 | 77.3 (83) | 58 | | | |

## Reading

- The rewrites are exact and cost nothing, but **scene draws fell only 2-13 ms/s (about 2%)**, not the ~60+ ms/s the plan hoped for. FPS is unchanged within noise (owner's settings 67.9/67.1 against 65.0/67.1; defaults 74.4/75.8 against 74.8/77.3).
- Why: in the real game the alpha control of a lit channel is mostly not lit, so the second `lightControl` call returned early anyway; the duplicate loop was cheap. The per-pixel surcharge (scene draws 301 to 427 ms/s) is the light loop itself over every pixel of every lit draw.
- The Task 6 gate (scene time fell by at least 60 ms/s yet FPS gained under 1) is **not met**: the precondition did not occur, so the critical-path hypothesis is neither confirmed nor refuted. It stays open until a change that really removes GPU time exists.
- Kept: all three changes are exact, no slower, and the owner's rule is to keep correct small wins.

## What would cut the per-pixel surcharge (not in this plan yet)

Per-light work that does not depend on the pixel (normalised attenuation terms, `rsqrt` of constants) is repeated for every pixel; precomputing it per draw needs a CPU-side change of `NativeLight`/`DrawUniform` in all backends and an exactness proof. To be decided after the motion-blur and copy work.
