# S2 — Scene boundary and motion vectors (DLSS/FSR)

Plan: `docs/superpowers/plans/2026-10-18-dlss-fsr.md`, spec `docs/superpowers/specs/2026-10-18-dlss-fsr-design.md`.

## Scene boundary

Evidence: `UC_SCENE_DUMP=<file>|400|120` (game/packet_dump.h) during `uctest run autoplay --god --speed 8 --skip-tutorials` (train
chapter from launch to the results screen, run `build/autotest/1792313482313855900`): 54 sampled frames, each with a screenshot
(`scene-<serial>.png`). Seven of them are the fixture `tests/data/scene_boundary/train_chapter.txt`.

Frame shapes (packet = draws up to one EFB copy):

| Frame | Packets | Structure |
|---|---|---|
| Title, menus, "game starts" prompt, results | 1 or 6 | empty copies of 640x360, one packet of orthographic HUD/menu draws (`80393B0C`, `803B7FE4`), present. No perspective draw. |
| Train gameplay | 19 | 0 empty; 1 opaque scene (`80366E14`, perspective, z-write) → copy 640x360; 2 transparent perspective effects → copy 640x360; 3 additive particles (`80359464`, `80357A68`) → copy 320x180; **4 no draws, copy 640x360** (the finished scene for the post chain); 5–16 post chain of full-screen orthographic quads (`800426C0`, `800421B8`, `80042470`, `80044340`) through 320x180 and 160x90 regions of the EFB, ending with the composite back to 640x360; 17 HUD (`8006CED4`, `8006D174` = reticle, `80393B0C`, `803B7FE4`, `80357A68`); 18 present. |
| Boss fight (scorpion) | 23–25 | as gameplay, but packets 1–4 are a glow pre-pass: perspective draws, a full-screen `800426C0` quad, blurs at 160x90 and a `800421B8` composite **followed by more perspective scene draws**. The scene copy for the post chain is packet 8. |

Rule (implemented in `gx::SceneBoundary`, `renderer/scene_boundary.h`): the scene ends at the first packet after a perspective draw that
has no draws and whose copy covers the last perspective viewport. All 34 frames with a 3D scene cut there with no perspective draw after
the cut; frames without perspective draws never cut.

Rejected: "the first orthographic draw after a perspective draw" cuts the boss frames inside the glow pre-pass (387 perspective draws after
the cut).

Consequences for the renderer:
- The scene viewport is 640x360 in the 640x480 logical EFB (top-left); the EFB height is `960*scale` for 480 lines, so the viewport is
  exactly the render-scale output extent. The upscaler works on that sub-rectangle.
- The scene copy of packet 4/8 must read the upscaled image: the upscale runs before that packet's copy, so the post chain and the
  HUD run at output resolution.
- Only perspective draws before the cut get the jitter; the pre-pass quads stay unjittered.
- Not recorded: in-engine cutscenes, later chapters, the pause menu over gameplay. A frame without the scene copy is upscaled whole at its
  present (HUD included); the renderer counts those frames.
