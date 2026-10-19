# S7: Tessellation survey (PN triangles / Phong)

Date: 2026-10-19. Tool: `UC_MESH_STATS=<file>` (`game/packet_dump.h`, `mesh_stats`). Runs: `train_gameplay --speed 8` and `fight --speed 8 --measure 10`
(`build/survey/{train,fight}-mesh.txt`, git-ignored).

## Method

Every scene (perspective) draw that uses a cached mesh (`DrawPacket::mesh`) is analysed once per distinct mesh. Vertices are grouped by position
(1 mm grid); a position shared by two or more vertices is a shared position, and it diverges when any of its vertex normals differs from the first by more than
15 degrees. Tessellating with per-vertex normals (PN triangles, Phong) moves the surface differently on the two sides of such a position and opens a crack,
so a mesh counts as crack-safe when at most 5% of its shared positions diverge. Triangles are counted per draw (drawn triangles), draws without a cached mesh
(inline vertices) are counted separately and never as safe.

## Results

| Run | Distinct meshes | Meshes over the 5% limit | Shared positions | Diverging positions | Drawn triangles in crack-safe meshes | Share |
|---|---|---|---|---|---|---|
| train (7 s) | 267 | 81 | 26,433 | 9,362 (35%) | 23,769 of 97,391 | 24.4% |
| fight (10 s) | 949 | 396 | 75,155 | 29,509 (39%) | 98.1 M of 235.3 M | 41.7% |

## Verdict: no-go

The gate asks for at least 60% of the scene triangles in crack-safe meshes. Both runs stay far below it (24% and 42%), and about 35-40% of all shared
positions carry normals that differ by more than 15 degrees: the game's meshes are hard-edged (split normals along creases). Tessellating them would need
crack repair (matching the displacement of split positions, or an edge-normal table per mesh) that this port has no data for. Task 21 (PN-triangle
tessellation) is dropped and phase 6 ends here. Revisit only with a mesh-side welding pass (position-keyed averaged normals for the tessellation factor
only), which would be its own spike.
