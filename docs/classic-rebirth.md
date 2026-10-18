# Classic REbirth owns the geometry

Read this before patching anything that draws. Classic REbirth's `ddraw.dll`
doesn't just wrap the game's DirectDraw output: it replaces the game's
geometry code with its own and draws through Direct3D 9. Patches inside the
replaced game functions do nothing, because that code never runs.

## What REbirth replaces

At startup REbirth writes a `jmp` into ddraw.dll at the start of these game
functions:

| Game function (`Dino Crisis.exe`) | What it does | REbirth's version (`ddraw.dll`, preferred base 0x10000000) |
|---|---|---|
| 0x5F18F0 | Builds the room meshes' triangles | 0x10054630 |
| 0x5F1AD0 | Builds the room meshes' quads | 0x100548F0 |
| 0x5F4760 | Winding test (the PS1 GTE's NCLIP), result at 0x6846B0 | 0x10054520 |
| 0x5F4FD0, 0x5F4D50 | Vertex projection (GTE RTPT/RTPS) | in ddraw.dll |

The file on disk still has the game's original code. To check whether a
function is replaced, read it in the running process. A replaced function
starts with `E9`, a `jmp`.

These game functions still run as the game's own code, and dc_modern patches
them:

- `RenderActiveRoomMeshesBySortMode` (0x44CED6): which room meshes are drawn
  (flag bits in byte +0x30), and their sort mode (+0x48).
- `ProcessSceneViewTransforms` (0x44C050): each mesh's view transform.
- The second room pass (0x44D52F), the camera callbacks (table at 0x645C1C)
  and the sort callbacks (0x5F1800–0x5F18E4).

## Drawing

REbirth draws the polygons with Direct3D 9 and sets `D3DRS_CULLMODE` to
`D3DCULL_NONE`. The GPU never culls; only REbirth's builders decide what
reaches the screen. In those builders:

- **Quads.** After the winding test, `jl` at 0x100549B4 drops polygons that
  face away from the camera.
- **Triangles.** At 0x100546D3, `ja` compares the winding result against
  0x1D4BF, unsigned. That one test drops polygons facing away and very large
  triangles alike.
- **Projection errors.** Both builders also drop polygons whose projection
  failed (bit 31 of 0x68464C).

## Why it matters for the shoulder camera

The rooms were modelled for the fixed cameras: many walls and fences exist
only on the side a fixed camera sees. From the shoulder those faces are often
seen from behind, and REbirth drops them, while their collision stays: an
invisible wall. While the shoulder camera is on, dc_modern turns the quad
`jl` into NOPs and the triangle `ja` into a signed `jg`, which keeps faces
seen from behind and still drops the huge triangles. When the fixed cameras
come back, the original bytes are restored. Both patches check REbirth's
bytes first and leave any other build alone.

The offsets are for the REbirth build this mod was made with. If REbirth is
updated, find the builders again: follow the `jmp` at 0x5F1AD0 and 0x5F18F0
in the running game.
