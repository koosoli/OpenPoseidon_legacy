# SINKHOLE-01 — W1: terrain holes (object-cut, ported from Malprave) drawn on wgpu

Branch: `dec/sinkhole-w1-terrain-holes`, on top of `dec/sinkhole-w0-texture-formats`.
Ledger: `WLD-HOLE-001`. Relates to `TERRAIN-CAVES-AND-BRUSH-20260909.md` (see below).

## What a terrain hole is

A model opts in, no world data needed:

* memory-LOD selections `terrain_hole`, `terrain_hole1` .. `terrain_hole8` — each the convex X/Z outline
  (3–16 points) of an area where the ground is cut away;
* `terrain_hole_hidden`, `terrain_hole_hidden1..8` — the same hole for everything **except drawing**
  (a cave footprint under a hillside: men, rays and AI go under the ground there, the hillside is still drawn;
  only the openings use the visible kind);
* a roadway LOD — the floor, stairs and ramps that replace the ground inside the hole.

Inside a hole the terrain gives no ground, no sea contact, does not stop bullets or shells, does not block line of
sight, and is not drawn (visible kind only); the object's roadway is the ground. WRP-placed objects work the same
as created ones. Holes are discovered lazily: nothing costs anything until a hole-cutting model has been seen.

## Why this design and not the SDF region of TERRAIN-CAVES-AND-BRUSH

That decision rejected a *shader-only* cave and recommended a local 3D density/SDF region replacing the
heightfield. Its rule — rendering, movement, projectiles and AI must agree — is met by Malprave's object-cut holes,
which hook every one of those consumers, and which exist, are tested in Dec's fork, and port with the hooked
functions unchanged here. A cave's walls and roof are ordinary model geometry, which is what an island port
(The Forest's caves are meshes) has anyway. The SDF region stays the better answer for *sculpted* terrain caves
and is not ruled out; it is simply not needed for content that ships its caves as models.

## Ported from Malprave (terrainHole1–8.patch, applied in order)

Clean: `TerrainHoles.cpp` (new), Landscape `RoadSurfaceY` (both overloads), Collisions `GroundCollision`,
`CheckIntersection`, `CheckUnderLand`, `ExplosionDammage` (a cellar roof shields), `AIUnit` `InsideBuildingHole`,
`OperMapDoor` `CreateField` (hole cells become space), `VehicleAIPilot` `CellarFormationPos`, `SoldierOldSim`
(men and bodies stay in the hole), `ClipDraw`/`ShapeLand`/`Tracks` decals, `CamEffects`, GL33 shader discard
(UBO binding 3; `PSNormal`/`PSDetail`/`PSGrass`).

Hand-merged against this tree (the hunks' context had moved on):

| Area | What this tree has that Malprave did not | Merge |
|---|---|---|
| Sea contact | water bodies + wave buoyancy in `GroundCollision` (WRL-003/006b) | hole check before the water block |
| World camera clamp | TW-WATER W5c `mayDive` split | `CameraFloorY(pos, focus, !mayDive)` feeds both arms |
| Free camera | TW-WATER W5c underwater free camera | floor = `CameraFloorY(.., false)` |
| Sound | `IndoorRoomAt` → `Audio::ClassifyEnvironment` (behind `POSEIDON_EXPERIMENTAL_EAX`) | `InTerrainHoleRoom` feeds `insideRoom`, **ungated** — no older behaviour exists underground |
| Decals | GPU terrain conform (`Object::PublishConformPlane`) | declines for an object down in a hole → CPU `DecalSurfaceY` fit |

## New: hole rendering on wgpu

`Engine::SetTerrainHoles(edges, n)` (edges `{nx, nz, d, last}` from `Landscape::TerrainHoleDrawEdges`, world X/Z,
≤ 64 per frame within 400 m) → `wgr_terrain_set_holes` → a 1 KB storage buffer, terrain `group(1) binding 5`:

* `fs_terrain` and `fs_terrain_prepass` discard inside a hole. Both must: the colour pass depth-tests EQUAL against
  the prepass, so a prepass depth over a hole would let the colour pass paint ground there. The planar reflection
  uses `fs_terrain` and so discards too.
* Grass placement (`cs_place`, mid ring, far ring) skips holes, from the grass module's own copy of the buffer
  (refreshed when the terrain's changes) — so the blades and their shadows are simply never placed.
* The buffer is written only when the hole set changes; `EngineWgpu` ignores the landscape's post-draw `nullptr`
  reset because the buffer is bound to the terrain and grass passes only.
* GTAO and the other screen-space passes need nothing (they read depth). The terrain sun-shadow sweep and sky
  visibility are left alone.

## Verification

* Build `win-x64-clang-rwdi` clean. Rust `terrain_hole_tests` (4): terrain.wgsl and grass.wgsl compose **and
  validate** with naga; colour and prepass both discard; all three grass placements skip; buffer size = shader
  struct. Rust `terrain::` 17/17; `grass::` 21/22 — the failure (`card_spacing_only_engages_with_a_resident_atlas`)
  is pre-existing on the base (the test still expects the pre-`grass.native` `cards_on_near`).
* C++ `PoseidonTests`: 3562/3629 pass; the 15 failures are all pre-existing on the base and in untouched areas
  (`I-05` shutdown audit, `test_asset_preview` saveToFile, `test_dev_panel_search`, `test_network_server`,
  `test_sim_stage_evidence` on `HarnessBuiltins.cpp`).
* In game (`.tw\test-holes.ps1` through the watcher; Malprave's `ugcave.pbo` via `--mod`; `UgCaveTest.Eden` on
  Everon): the engine logs "ugcave.p3d cuts 6 hole(s) in the terrain"; a man moved into the chamber stands and walks
  on its floor at **exactly ground - 5.000 m** for 8 s, alive, and returns to the surface. In an earlier run
  Malprave's EAST squad left in the chamber engaged the player through the stairwell (LOS and fire work through
  the hole). Evidence: `docs/roadmap/evidence/sinkhole-w1-20261003.json`.
* **Found on the way and fixed (`0cea4b3b`):** `Model::compile` ignored `autocenter=0` unless the *first* LOD carried
  it; O2-authored models carry it on the geometry LOD, so every such MLOD was silently recentred -- the cave stood
  2.35 m too high (floor at -2.65 m). Now honoured on any LOD; regression test `[sinkhole][autocenter]`.

## Owner play-test round 1 (fixed)

The owner played the cave: "the roof of the cave is bleeding light, the roof is transparent, I can see the grass
and soldiers' feet above; when inside I can see rendering lines and flashing". A/B captures inside the cave
(GPU occlusion on/off, grass off, shadow maps off, GL33) separated it:

* **Hi-Z occlusion:** not involved (identical captures with `WGR_GPU_OCCLUSION=0`).
* **Sun inside the cave:** the heightfield is never a shadow caster for objects, and the long-range terrain shadow
  mask marches *outward* from each column, never counting the column's own ground — anything less than ~3 m under
  the surface read as open sky, so the ceiling (0.4 m down) was in full sun while the walls stayed black. With
  grass off, the bright dapples on the ceiling vanished: they were the shadows of the grass on the ground above,
  swaying in the wind — the flashing. **Fix:** `shade()` removes the direct sun from a fragment 0.05–0.25 m
  (ramped) or more under the terrain surface at its own xz, read from the conform heightmap both object pipelines
  already bind (`0aff777e`, ramp tightened in `47453f69`). Openings lose their sun shafts.
* **Holes bypassed the settings queue:** `EngineWgpu::SetTerrainHoles` called the renderer from the landscape draw,
  racing the render thread (REN-THR-013); now queued with the other producer-side pushes. A log line reports the
  edge count when it changes ("Wgpu terrain holes: 8 edge(s)" in the test cave: two visible openings; the
  footprint uses Malprave's hidden holes, so the ground over the cave stays drawn — correctly with grass on it).
* GL33 (Malprave's original hole path) renders the same cave evenly dim and enclosed, which is what wgpu now does
  apart from Oli's darker interior ambient.

## Owner play-test round 2 (fixed / measured)

* **"The island surface goes black and all objects appear floating"** while a mission loads, switches or exits.
  A hole object whose transform is momentarily collapsed maps every hole point to one spot; each zero-length edge
  test reads 0 >= 0, so the hole contained every point — the terrain draw discarded the whole island and the
  gameplay queries gave no ground. `MakeHoleArea` drops a footprint under 0.01 m² (or non-finite), and the terrain,
  grass and GL33 tests treat a zero-length edge normal as outside (`e65e45de`). Captured: deleting the cave object
  mid-mission leaves the terrain whole and the openings close.
* **"Soldiers also illuminated and not dark":** the interior-sky term darkens static buildings it has rooms for,
  not a soldier in a cave. `shade()` scales the sky ambient to 0.12 on the same underground ramp (`e65e45de`);
  captured dim.
* **"The cave's stairs seem to be shimmering":** a 4-frame burst at 0.2 s on the stairs from inside, static
  camera, after the round-1 and round-2 fixes: the only changing pixels are the animated soldier's (mean |Δ| ≤ 0.05
  of 255 outside him). Not reproduced in the darkened cave; owner re-check pending.
* Seen on the way, not fixed: a man left inside a hole when its object is deleted stays where he was, under the
  ground (`LeavesTerrainHoleUnderground` only acts on movement).

## Not done / not verified

* **Box3D heightfield** (`LooseObjects`, `PhysicsWorld::SetTerrain`) and shell penetration (`ShotShell::TryPenetrate`)
  still see solid ground over a hole: Oli's wrapper has no per-cell hole material. Only the opt-in loose-object
  physics and penetration are affected; ordinary collision, bullets and LOS use the hooked paths.
* **Enfusion CPU clutter** (`TerrainWgpuEnfusionClutter.cpp`, Reforger worlds only) is not hole-aware.
* Malprave's own limits stand and are W2's job: a hole object is found only within 25 m of its origin, at most 8
  hole selections per model, convex outlines only; AI paths do not join across separate cave pieces.
* `diag_camera` ignoring holes (Malprave ug10) is moot here until the diagnostics port lands (DIAG-001).
* The cave interior renders near-black (Oli's interior-sky lighting treats it as enclosed rooms), so the hole
  edges and walls are not yet visually confirmed; a light source for the check is still to do (W5 needs one anyway).
* Owner play-test pending.
