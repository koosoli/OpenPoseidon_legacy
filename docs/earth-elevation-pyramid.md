# Whole World elevation as a tile pyramid (design, 2026-10-24)

Today the Earth's ground is drawn from **window patches**: `EarthTerrainStream` builds a 257 x 257 near grid and a
horizon grid around the camera, `TerrainWgpu` turns them into two meshes (built off the main thread, uploaded on it)
and installs the near grid as the renderer's heightmap. Every recentre (about once a second in flight) means a new
patch, two new meshes and a heightmap install, and the detail is fixed per window: the horizon pops when the next
window arrives. The imagery already left that model (`EarthTileCache`, the imagery pyramid); this is the same for
elevation, after bevy_terrain's split of *data LOD* and *geometry LOD* and Cesium's quadtree terrain.

## What the patch is for, and what stays

The patch is the synchronisation point for a lot more than drawing: gameplay ground (`PublishGround` /
`QueryGround`), the renderer heightmap (terrain shadow mask, sky visibility, the object shaders' cave test), grass,
canopy trees, and the ground features, buildings and worldgen stand on. **The patch stays as the CPU gameplay and
content grid.** What goes is drawing the ground from it: the near and horizon meshes, their builds and uploads.

## Data: `EarthHeightCache`

* Terrarium tiles (zoom 0..15), decoded, with the rules the patch builder applies baked in: the sea's depth from
  zoom 10 where a finer tile is the open sea's 0 m fill, the WorldCover shore rule near the camera, and the OFP
  islands (`EarthIslands`).
* Stored on the GPU as an RGBA8 atlas of 256 x 256 slots, **Terrarium-encoded** (exact, and RGBA8 already has every
  path: creation, region update, the registry), read with `textureLoad` in the vertex stage. No borders: a texel
  past a slot's edge is looked up in its neighbour.
* A lookup texture like the imagery's: per zoom a 16 x 16 window round the camera, slot or empty.
* Planned from the geometry: a drawn node of zoom z wants the data tile of zoom z - 3 that contains it (its 32
  vertex spacings are that tile's 32 texels). Missing data falls back to the nearest resident ancestor.

## Geometry: CDLOD on the mercator quadtree

* Nodes are web-mercator tiles; each is one draw of a shared 33 x 33 grid mesh (uploaded once). The CPU selects nodes
  per frame by distance (CDLOD ranges, children replace a parent inside its range) and draws ~200-600 of them; a draw
  costs ~0.2 us, so no instancing is needed.
* The vertex shader (shader3d `vs_main`, conform mode 3) places a vertex from the node's parameters, which ride in
  the draw's world matrix and conform vectors (no new per-draw interface): the node's west longitude and north
  mercator Y relative to the origin, its spans, its data tile and morph range.
* Precision: the world frame is the sphere's orthographic projection on the origin's tangent plane
  (`EarthFrame.hpp`). The shader computes it from *differences* -- `sin(dp)`, `2 sin^2(dl/2)`, the Gudermannian
  difference for latitude -- so a point near the camera is exact to millimetres in f32, and the far ones to well
  under a pixel.
* Heights are bilinear in the data tile; CDLOD morph to the parent's grid near the outer range, so there are
  neither cracks nor pops.
* Imagery: each node's UVs are its zoom-19 mercator offsets from its own corner, exactly what the tile-mode ground
  shader takes (`reflect0` = the corner, mode 1).

## Phases

1. **Data:** `EarthHeightCache` (worker, atlas, lookup, baked rules), its binding in the conform group, the decode
   in the shader. No visible change.
2. **Ground from the pyramid**, behind `POSEIDON_EARTH_HEIGHT_PYRAMID=1`: node selection, the grid mesh, conform
   mode 3. The near and horizon meshes are no longer drawn (still built at first), the patch still feeds gameplay
   and the heightmap. Compare against the meshes: heights, seams, imagery, the sea, the islands; a flight for cost.
3. **Default on**, and stop building the near/horizon meshes.
4. **The heightmap install**: terrain shadows and sky visibility from the pyramid, then the patch becomes
   gameplay-only (no renderer install at all).

Known differences to watch: gameplay ground (patch, bilinear at its grid) vs drawn ground (pyramid, bilinear at
data texels, morphing) differ by centimetres on smooth ground, more on steep slopes; soldiers' feet are the test.

## Status (2026-10-24 night: phases 2 and 3 done; the pyramid is the default)

`POSEIDON_EARTH_HEIGHT_PYRAMID=0` brings back the patch meshes; otherwise they are neither built nor drawn (a window's
imagery atlas, which waited for its meshes' UVs, now goes in at once). Measured with `Run-EarthLadder.ps1` (Alps
46.537/7.962 at 700/2500/9000 m, Helgoland 54.182/7.886, `-FlyPerf` with `WGR_GPU_SPIKE_MS=0` for per-frame GPU time):

- **Look:** the same ground as the patch meshes at all three heights, without their jagged stair-steps on far ridges;
  the Helgoland coast and sea identical (the rectangle and bands in its sea at 2500/9000 m are in both).
- **GPU, whole frame, flight:** median 9.3 ms, p95 14.1 -- the patch meshes 11.3 / 15.4. The first version cost 18.2 /
  30.2: each vertex placed itself four times and looked its height up three times (a finite-difference normal). The
  normal now comes from the slope of the same four texels, on the globe's east/north there; the look is the same.
  (An earlier note here said "level with the meshes" from `swap` time: that hides GPU cost under vsync. Use the
  GPU timers.)
- **Main thread:** the node pass is 0.38-0.82 ms a frame (`Earth ground nodes:` every 5 s, parts listed) for ~1300
  nodes; it was 0.9-1.9 before the selection took each row's and column's trigonometry once a frame.
- **Traps found:** the depth prepass culls back faces whatever a draw's double-sided flag, so the grid must be
  clockwise from above or the nodes are in colour but not in depth and the sea (tested against that depth) covers
  the ground. Neighbouring nodes part by a hair along their shared edge (different f32 origins): skirts hide it.

Phase 4 (terrain shadows and sky visibility from the pyramid, the patch gameplay-only) is next.

## 2026-10-25 night: the ground feet stand on, view culling, the sea beyond the window

- **Feet:** the pyramid's z15 texels and the gameplay patch (z14 on a 6.25 m grid) differed by a median 0.65 m near
  the Alps origin (0.25 m on 10-25 degree slopes, 0.9 m over 40, up to 1.9 m; measured offline from the tile caches,
  scratchpad `feet_gap.py`). Inside the gameplay window the nodes now take the patch heightmap's own surface
  (`bare_surface_y`, Landscape::SurfaceY's triangles) and a normal from a cell either side, blended into the pyramid
  over the window's last 40 cells -- conform mode 3.5, only while the installed heightmap is this origin's patch
  (`POSEIDON_EARTH_NEAR_BLEND=0` turns it off). Soldiers on Alpine slopes stand on the drawn snow.
- **Culling:** nodes outside the view are not submitted (their data is still asked for, so turning round finds it
  resident). A node is a vertical segment from its data tile's lowest to highest height (`EarthHeightCache::
  HeightRange`), widened by its half diagonal. Earth ground is not a shadow caster, so this is exact. About two thirds
  of the nodes go. `POSEIDON_EARTH_NODE_CULL=0` draws all.
- **The sea beyond the window:** Tidewater's floor outside the heightmap's window was a 200 m stand-in, which drew the
  window as a bright rectangle on a shallow sea (Helgoland at 2.5 km). The patch's horizon heights now go to the
  water as a 512 x 512 grid (`wgr_water_set_far_floor`, `POSEIDON_EARTH_FAR_FLOOR=0` off), handed over inside the
  window's last 400 m.
- **Skirts** hang 1 m, not half a grid cell.
- **Flight (Alps, `-FlyPerf`, nothing else running):** GPU median 7.3-7.6 ms, p95 10.8 (patch meshes 11.3 / 15.4);
  CPU average 2.1 ms (meshes 1.65), worst frame 51-63 ms (meshes 110). The first run after a shader change compiles
  pipelines at runtime and hitches up to 1 s; the second run does not.
- **Checked:** the origin re-centring in fast flight, teleports (Helgoland, Grand Canyon, Everest, back), the deep
  Atlantic floor (-4.9 km at 45 N 10 W).
- **Known, not fixed:** above 8 km the Earth sea is not drawn and the sea is the imagery, whose zooms (Esri, and EOX
  where Esri fails) disagree about the sea's colour: rings round the camera. A faint dark diagonal in the clear
  shallow sea off Helgoland's dune predates tonight.
