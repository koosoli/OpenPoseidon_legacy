# Sideways caves and terrain brush footprint

## Current implementation

TerrainBrush::PaintTerrain changes a rectangular patch of scalar heights and
patches PhysicsWorld's heightfield before calling Landscape::HeightChange.
Landscape::SurfaceY interpolates one height for each X/Z coordinate. There is no
second surface for a cave floor beneath a retained roof. A deeper height stroke
therefore makes an open crater, not a sideways cave.

The cursor preview now shares TerrainBrushTargetAtPixel with the painting path,
uses the effective radius (including the 1.5-cell minimum), and follows SurfaceY
with a bounded 96-segment screen overlay. Green means raise, amber means lower,
red means the target cannot be edited. This is an editor overlay, not depth-tested
geometry; objects can lie in front of the outline. Settings expose Show brush
radius at cursor, enabled by default and restored by existing settings resets.
The current 2049-sample editing limit is surfaced explicitly in the panel.

## Why not a shader-only cave

- WgpuRenderer/TerrainWgpu.cpp and rust/src/terrain/terrain.wgsl render heightfield
  geometry; deleting colour fragments alone would leave depth/shadow/other passes.
- World/Simulation/Collisions.cpp::IntersectWithGround and Landscape::SurfaceY
  still see the solid hill. Shots.cpp uses IntersectWithGroundOrSea for impacts.
- SoldierOldMove.cpp uses RoadSurfaceY/SurfaceY for standing/movement heights.
  A Jolt-only collider change would not replace these gameplay decisions.
- PhysicsWorld::SetTerrain/PatchTerrain accept one scalar height per cell.
  RegisterStatic consumes model convex Geometry LOD components, not an editable
  concave terrain mesh. Adding a second collider without removing the original
  heightfield collision does not create usable empty space.

No fake sideways-hole switch was added. Rendering, player movement, projectiles
and physics must agree before calling a cave playable.

## Recommended experiment, not implemented yet

Keep the original heightfield outside explicitly edited small regions. Inside
one region use a bounded 3D density/SDF volume, initially representing the solid
hill. Subtract a spherical brush along the cursor ray and rebuild only changed
mesh chunks plus a one-cell seam halo. Replace, not overlay, the corresponding
heightfield surface/collision. Avoid a full-world voxel allocation.

1. Offline meshing tests: hill minus sphere/tube, watertight boundary seams,
   deterministic output, real roof/floor geometry and a bounded memory budget.
2. One opt-in single-player scene with matching rendered and static collision
   meshes. Introduce altitude-aware surface/raycast queries and route infantry,
   projectiles and physical probes through them within the region. Outside it,
   retain the existing calls/results. Acceptance: walk and fire through the
   entrance, stand on both roof and floor, hit ceiling/walls and roll probes in.
3. Wire the editor's spherical brush and live mesh/collision replacement only
   after those tests pass; measure per-stroke latency and rollback on failure.
4. Add save/export and deterministic edit replication separately. Do not imply
   legacy campaign scripts, AI paths, multiplayer or old saves support caves
   before they are integrated and tested. Snow, water and lighting queries also
   need region-aware treatment, not automatic underground precipitation.

This is a local 3D-terrain extension, not a small change to the radius slider.
It remains an open architecture/implementation task; only the radius preview
and explicit editor limitations are implemented in this change.
