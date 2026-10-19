# S6: Graphics survey (lighting, game shadows, normals)

Date: 2026-10-19. Tools: `UC_LIGHT_STATS`, `UC_PACKET_STATS`, `UC_SKIP_CALLSITES` (Task 1 of `docs/superpowers/plans/2026-10-19-graphics-enhancements-p0-3.md`).
Data: `build/survey/{train,fight}-lights.csv`, `build/survey/{train,fight}-packets.txt` (git-ignored). Runs: `train_gameplay --speed 8` (7 s of gameplay, 808 mesh draws) and `fight --speed 8 --measure 10` (the whole run including the 8x lead-up: 3.08 M mesh draws, 387.6 M scene vertices). "Scene draw" = a draw whose `summarize().perspective` is true.

## Findings

| Question | Answer (fight run) |
|---|---|
| Where do scene vertices come from | One callsite, `80366E14` (the mesh draw): 381.4 M of 387.6 M vertices (98.4%). Next: `80357A68` 0.5%, `8015B668` 0.3%, `800BAF88` 0.2%. |
| GX-lit versus baked colour | 96.2% of the `80366E14` draws have colour channel 0 lit (2.96 M of 3.08 M), 52.7% also channel 1. The small callsites `8015B668`, `8015B768`, `8015C888` (lit, additive-style blend 1:4:3) are lit too; `80357A68`, `80359464`, `8035B4CC`, `803595B0`, `8035C4E8`, `8035BFC4` (additive or alpha, no lighting) are baked/unlit. |
| Light kinds (counted per lit draw and channel) | directional 2.79 M, point 5.01 M, specular 1.54 M, **spot 0**. No spot light appears in either run. |
| Stable dominant directional light | Directional lights are on 60% of lit channels (2.79 M directional vs 4.58 M lit-channel draws). Whether one directional light stays dominant across a scene is not measured here (the CSV aggregates over time); `select_dominant` (Task 11) handles it with hysteresis. The train car is lit by point lights plus a directional fill. |
| Game's own shadows | **Not confirmed.** Candidates: `800BAF88` and `800BADD8` (27,271 draws each, 34 and 18 vertices per draw, unlit, alpha-tested, alpha blend, no depth write, textured, with normals). A fight capture with both skipped (`UC_SKIP_CALLSITES=800BAF88,800BADD8`) landed on a different game moment (smoke after an explosion), so the two frames cannot be compared. Confirm with a replay of a recorded demo (identical guest work) and frame captures at the same retrace before phase 4. The train gameplay capture is a fade-in (black) and shows nothing. |
| Opaque / alpha-tested / blended | `80366E14`: 84.3% of draws write depth (opaque or alpha-tested casters), 35.0% use an alpha compare other than "always". Every draw has blend mode 1 set in the state (src-alpha), so blended-versus-opaque cannot be told from the blend state alone; depth write is the proxy. |
| Normal coverage | 100% of the `80366E14` vertices carry a normal (381.35 M of 381.35 M); the unlit particle-style callsites carry none (`80357A68`, `80359464`, `8035B4CC`, ...). |
| Tessellation candidates | Lit, triangle, depth-writing mesh draws with normals: about 84% of the `80366E14` draws, i.e. about 83% of all scene vertices. Distinct mesh count and shared-edge cracks are not measured; they decide whether phase 6 ships. |

## Verdicts for phases 4–6

- **Shadow maps + PCSS (9): go.** One callsite carries the casters; directional lights exist. With no spot lights, the shadow-casting light must be a directional light; if a scene has none, there is no shadow (design: "directional or spot").
- **Contact shadows (6, phase 3): go**, same light source rule; with only point lights it does nothing (tested in Task 13).
- **Game's own shadows: unresolved.** Confirm `800BAF88`/`800BADD8` by demo-replay capture before shipping shadows; if they are blob shadows, suppress them by callsite when real shadows are on.
- **Volumetric lighting (7): go with change.** The dominant directional light can drive shafts; point lights have no shadow map, so shafts from points use the screen-space variant only.
- **Motion blur (8): go.** Vectors already exist for 98% of vertices (single callsite).
- **Tessellation (11): go, gated.** Candidate share is high; ship only after a crack check on the shared-edge meshes in a demo replay.
