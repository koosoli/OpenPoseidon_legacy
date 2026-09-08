# Experimental snow and terrain deformation

## Scope

Owner-requested, off by default. Weather tab > Experimental snow enables a
mission-local terrain layer. Accumulation rate, maximum depth, add 10 cm and
clear controls are independent. Snow replaces rain particles while enabled;
wind and the existing precipitation occlusion path apply to the flakes.

SnowField stores sparse 2 m chunks at 12.5 cm resolution. Footstep events,
independent car/tank contact trails and nearby explosive impacts depress this
layer. Neither elapsed time nor camera movement deletes depressions. Only new
precipitation fills them, including when undisturbed snow is at maximum depth.
The 32,768-chunk bound rejects new chunks rather than evicting old tracks.

The GPU receives a 512-square, 64 m camera window at at most 10 Hz (about 1 MiB
per upload). The global snow depth applies beyond it; fine track detail fades
at its edge and returns on revisiting. Terrain vertices rise; fragment normals
and albedo show finer detail than the existing mesh can geometrically resolve.
Frustum bounds include the displacement. Disabled snow avoids field updates
and sampling, but retains the approximately 1 MiB GPU buffer.

## Limits, not completion claims

- WGPU terrain only, not snow on roofs, objects, roads or vegetation.
- Ground under roofs still accumulates: flake occlusion is not an accumulation mask.
- Terrain collision, wheel/foot positioning, navigation and movement resistance
  are unchanged. Deep cover can engulf feet/wheels. This is not physical deep snow.
- Tracks are mission-local, not saved or replicated over multiplayer.
- No melt model or snow redistribution; excavated snow does not form banks.
- Fine depressions are normal/albedo detail on the existing coarse terrain mesh.
- Existing smoke containment, corpse/wave alignment and cloud temporal artifacts
  remain separate open work; this experiment does not resolve them.

## Verification

C++ snow/CDLOD tests: 3,823 assertions across ten cases passed. Covers negative
coordinates, camera round trips, no age decay, capped-depth snowfall filling,
interpolated trails, teleport rejection, reset and invalid input guards.
Native C++/Rust build passed before the final culling-margin change; final build
and installed-game visual acceptance are pending in this checkpoint.

## Destructible terrain feasibility

Heightfield craters are feasible; caves and overhangs need a different geometry
representation. Landscape::HeightChange currently only calls SetData, with its
FlushCache commented out. Calling it from grenade impacts is therefore unsafe.

A proper implementation needs bounded dirty regions covering CPU height and
physics collision, GPU heightmap uploads, CDLOD min/max hierarchy, terrain sun
shadows and sky visibility, roads/vegetation grounding, AI oper/lock caches, and
save/network authority. Updating only the rendered height would create invisible
collision; rebuilding a whole map per explosion would be too expensive. Snow is
intentionally separate and must not be described as destructible ground support.
