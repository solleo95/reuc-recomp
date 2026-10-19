# Ray-traced shadows of the level (design)

Date: 2026-10-19. Status: draft for the owner's review. Builds on `2026-10-19-ray-tracing-design.md` and phases 0–2 of its plans (RT shadows of characters are in; see `docs/spikes/S9-rt-shadows.md`).

## Intent

The owner wants the environment to shadow and be shadowed: a seat blocking a lamp, a pillar shading a zombie, a table shading the floor. After phase 2 only skinned draws (characters, enemies) cast ray-traced shadows, because tracing the characters' lights against the whole level turned every character black indoors. This design makes the level take part, using the game's own lights where they are physical and leaving them alone where they are not.

## What the game's lights are (measured 2026-10-19, fight3 demo and the first scene 1–3 s after the chapter starts)

| Light | Where | Looks like | Share of draws |
|---|---|---|---|
| **Point lights (kind 1)** | 9–25 per frame, view-space positions along the train: y 2–5 above the floor, spaced along the car's length, warm (185,155,60), (165,145,50), orange (195,75,0), green (0,50,0) at x ±16 | the car's lamps and sconces: physical | static draws ≈ 1.0 M point-only + 0.9 M directional+point; skinned ≈ 0.8 M point-only + 0.6 M directional+point (of ≈ 3.1 M records in the demo) |
| **Directional lights (kind 0)** | two per character group, colour (29,29,29), far point ±1e9 | a character rig: weak fill lights that follow the character, not the level | skinned and many static draws |
| **Specular lights (kind 3)** | one per rig, colour (255,255,255) | the specular channel's highlight light | channel 1 of ≈ 45% of the draws (`ch1 lit`, attenuation function 1) |

Consequences found in the live frames:

1. A ray from a character toward a rig light leaves the room, so the ceiling occludes it: the character goes black. Rig directions are not places in the level.
2. The point lamps are in the level, so rays toward them are meaningful: a seat or wall between a surface and a lamp is a real shadow. A ray that ends inside the lamp's own fixture (shade, chandelier) is blocked by the fixture, so the end of the ray must stop short of the lamp.
3. About a third of the records are model-invalid for the one-channel factor because the TEV adds the rasteriser colour of **channel 1**, which is the specular channel (`0x8F8AF` + `0x28F8A0` with stage 1 reading channel 1, `ras` added by stage 2 in other shapes). It is a highlight, proportional to the same lights' visibility, not an independent colour.
4. Rebuilding the acceleration structure every frame costs ≈ 0.9 ms end to end (S8/S9), which already takes about a third of the High budget; adding the static level makes the structure roughly ten times larger.

## Design

1. **Light classes for shadows.** Per pixel the pass classifies each light of its record: *lamp* (point or spot function), *rig* (directional), *specular* (attenuation function 1: never traced, its share stays). Lamps follow `rt_local_shadows`, rigs follow `rt_shadows` (the options keep their names; their meaning is documented as "lamps" and "character rig lights").
2. **Two occluder classes with instance masks.** The structure holds two instance classes per scene packet: *static* (non-skinned draws) and *skinned* (per-vertex matrices). Instance mask bit 0 = static, bit 1 = skinned. A lamp ray uses mask 0x3 (the whole scene), a rig ray mask 0x2 (characters only: their shadow on the level and on each other, never the level's own walls). The ray's `TMax` for a lamp is `max(distance − fixture clearance, 0)` (a constant in `rt_shadow_math.h`, tuned on the lamps in the first scene).
3. **Channel-1 specular in the TEV classification.** `light_model_valid` accepts reading the rasteriser colour of channel 1 when channel 1 is lit with the specular function only; the record carries a flag. The factor, computed from channel 0's diffuse lights (as now), scales the whole colour, specular included (a highlight in shadow dims with the diffuse part; a known approximation, noted in the code).
4. **Static geometry from a cache, not per frame.** A static draw's mesh is immutable and shared (`gx::MeshBuffers`, resident in `MeshSlab` buffers) and its position is one palette row (object to view). So its BLAS is built once **in object space** and cached by mesh identity; the TLAS instance carries the draw's 3×4 row as its transform. Per frame the static cost is the TLAS build over about a thousand instances, the transform copy and nothing per vertex. Skinned draws stay per-frame view-space BLAS as today. The cache is bounded (a byte budget, least recently used out) and tied to the mesh slabs' lifetime.
5. **Receivers and noise as before.** Lit, model-valid pixels; the own denoiser; soft lamp shadows with a disc of `kRtLightRadiusFraction`. Alpha cut-outs remain non-occluding.

## Decisions for the owner (with recommendations)

1. **Rig lights shadow only from characters** (recommended) versus tracing them against the level too (rejected: black characters). The level's shadows come from the lamps.
2. **Specular highlights dim with the diffuse part** (recommended, simple) versus a separate visibility term for specular (more records and shader work, small visible gain).
3. **Static-geometry cache now** (recommended: without it the level doubles or triples the per-frame cost the High budget cannot afford) versus per-frame rebuild first and cache later.

## Goals

- The level's lamps cast shadows from seats, pillars, tables and characters onto lit surfaces; characters stay normally lit indoors; matched live frames show it.
- High (all RT options) keeps the owner's 100 FPS budget: the structure cost with the cache is at or below today's; the shadow pass itself stays within the S9 numbers.
- RT off, or both shadow levels off: nothing allocated or recorded (as now); the cache exists only while RT shadows are active.

## Non-goals

- Shadows from the rig lights onto the level; light baking; world-space lights (the game has no world matrix); alpha cut-out shadows; DLSS Ray Reconstruction; the other RT effects.

## Risks

- **Lamp fixtures.** A shade or chandelier around a lamp blocks every ray to it until the clearance exceeds its size; one constant may not fit every fixture (the first scene has lamps at two heights). Mitigation: measure the fixtures' extents against the lamp positions on both scenes; a per-draw clearance from the nearest occluder only if one constant fails.
- **Instance transforms.** Draws sharing a mesh with different matrices (flipped or scaled models) need the transform exact in float; a mirrored transform flips triangle winding (the ray flags must not cull by facing).
- **Cache memory and churn.** A level's meshes may exceed a sensible budget; eviction must not stall (BLAS builds spread over frames, a missing BLAS means the draw does not occlude for that frame).
- **Specular approximation** may over-dim highlights; judged on live frames.

## Left to the builder

- BLAS granularity per mesh (one per `GpuMesh`), the cache's key and eviction policy, the build scheduling, scratch pooling.
- The exact clearance and lamp-radius constants, tuned on the two scenes.
