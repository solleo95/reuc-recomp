# M8: can a mod move the game's camera? (free camera, first spike)

Checked 2026-10-23 (USA build, train chapter, harness `uctest run camera_capture`). **Result: not yet.** The camera API is read-only, and the one matrix it reads is not what the game renders with. A free camera needs a decision about where to cut in; the options are at the end.

## What was found

- `camera.view` / `position` / `forward` read the 3x4 matrix at `0x8044B4E0` (M1 note). It is written by `hle_PSMTXCopy` called from the translated function `8013D35C` (the call at `8013D410`, return address `8013D414`, caller chain `801F5550`, `8003D348`, `8003F820`, `800041A4`), found with `UC_WRITE_TRAP`. The function builds the matrix from the rotation and position values of the object at `0x8043A720` (`fn_8037D38C`, `fn_8037CE54`, `fn_8037D32C`, `fn_8037D2BC`) and then copies it to `0x8044B4E0` and a 64-byte block next to it.
- **Overwriting that matrix changes nothing on screen.** A spike in `hle_PSMTXCopy` added 60 units, then 5000 units (500 m), to the translation of the matrix right after the game wrote it: the captures are the same as without it. So the renderer's camera does not come from `0x8044B4E0`; the matrix serves the game's own use (aiming and world-to-screen).
- No other copy of the matrix exists in memory: a scan of MEM1 and MEM2 for the first row (16 bytes) of the matrix finds no second occurrence in the same frame.
- The `PSMTXConcat` calls of a scene (60 000 sampled) are skeleton animation: no single address is the first argument of most calls, so the view matrix is not simply "the first matrix of the model-view concatenation".
- The hook API cannot reach this code today: only `PlayerEventHandler` and `GameWeaponUpdate` are hookable, and a hook point is compiled into the translated code (a new one needs a schema change and a rebuild).

## Options (for the owner)

1. **Cut in at the renderer.** Every 3D draw has a model-view matrix; the camera moves if each perspective draw gets `D * modelview` with `D = V_new * V_old^-1` (HUD and other orthographic draws untouched). Needs the game's real `V_old` (the part still unknown: the draws' matrices are already combined) or, without it, a delta applied in eye space (`D` = a move and turn of the eye, which does not need `V_old`: "the camera moved by D relative to where the game put it"). Light positions and directions, fog and the normal matrix must get the same `D`; ray tracing, DLSS/FSR frame generation (camera reference, motion vectors) and the path tracer read the same matrices and must agree. This is a renderer feature with a mod API (`camera.offset`) in front of it.
2. **Find the render camera in the game.** Locate where the game combines its camera with each object (a `GXLoadPosMtxImm` caller chain read from a packet dump, then its source object) and write the eye, target and up there, in a new hook or an HLE wrapper. Consistent for everything (lights, culling, sky), but needs more reverse engineering, with an unknown result.
3. **A "look-around" camera only.** A rotation delta in eye space (pitch and yaw around the eye) applied in the renderer: a small version of option 1 with no translation, so no light or culling problems beyond the edges of what the game draws.

Recommendation: option 1 in its eye-space form (`camera.offset` = a move and a turn relative to the game's camera), step by step: first rotation only (look around), then translation. It reuses nothing of the guest and cannot crash the game. Option 2 only if the owner needs lighting and culling to follow a far-away camera.
