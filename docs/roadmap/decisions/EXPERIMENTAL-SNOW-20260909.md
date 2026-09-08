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
The final snow/CDLOD/rain run passed 3,863 assertions across 17 cases, including
filling all 32,768 storage chunks and proving overflow does not evict old tracks.
Native C++/Rust build and installed-game validation passed. Code commits
587824b3 and 5453ac8d were pushed to both safety refs before deployment; the
installed matched EXE/DLL pair is 5453ac8d.

Private `.tmp-bulb/snow.Intro` uses an actual walking SoldierWB, a driven Jeep,
and a GrenadeHand on the dry Desert plateau at 9500/3500. `snow-persistent.png`
at 70 seconds shows boot prints, both wheel ruts and the exposed grenade circle
after the camera moves to x=12000 and returns. `snow-refill.png` at 70 seconds
with new precipitation at 0.3 m/min covers the old marks and grenade circle.
`snow-off.png` shows the original dry surface. `snow-final.png` checks the
corrected native additive Flare0 blend (the initial alpha blend made dark
rectangles) and shared snow normals in the colour and prepass shaders. Flakes
remain stylised screen-space sprites, not physically lit snow scattering.
All five capture processes exited 0, without shader validation/script errors.

Last seven terrain GPU samples: disabled median 0.326 ms; final snowfall 0.357 ms
(prepass plus colour), at 1280x720 on RTX 3070 with DLSS. This is a short separate-
launch scene comparison, not a universal frame-cost guarantee or a 4K benchmark.
The Weather controls compile and bind directly to the tested field; actual
interactive slider operation and long-session/high-density stress remain unverified.

Final integration check: installed 94101879 after rejecting the separate cloud
experiment; `.tmp-bulb/snow-installed-final.png` again exits 0. Combined targeted
snow/CDLOD/rain/water/cloudlet tests pass 3,938 assertions in 28 cases; sky WGSL
validation and the renderer-ledger validator also pass. Cargo.lock retains its
original foreign-change SHA256 throughout.

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
