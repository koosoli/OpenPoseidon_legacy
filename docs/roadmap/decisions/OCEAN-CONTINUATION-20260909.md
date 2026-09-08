# Offshore ocean continuation (2026-09-09)

## Scope

The authored terrain is finite, but the conspicuous straight offshore boundary
also came from a finite renderer water root. Nogova's logged water core covers
[-19200, 32000] metres on both horizontal axes. Increasing the stored quadtree
extent would increase initialization, terrain scans and memory unnecessarily.

Installed d243cae4 instead selects procedural, flat CDLOD roots outside that core.
Their lattice is anchored to the existing core, not translated with the camera.
The camera's far distance and frustum limit visited roots/descendants. Root (0,0)
is excluded, leaving the terrain-aware core and shoreline pruning untouched.
Selection and morph bands match the stored tree's algorithm. Outside roots use
open-ocean metadata, conservative wave bounds and the existing water draw queue.
No new FFT, heightmap, simulation field, GPU pass or terrain/physics extension.

Default enabled. Developer A/B override: WGR_OCEAN_CONTINUATION=0. This switch is
not a fog workaround; disabled fog remains disabled. The normal camera far plane
and floating-point precision limits still apply. This does not invent missing
land, extend authored islands, or solve every seabed/shore/reflection seam.

## Verification

- PoseidonGame and PoseidonTests built successfully.
- `[cdlod]`: 3722 assertions / 7 cases pass. Procedural selection is compared
  field-for-field with a stored flat tree across camera distances, elevations,
  negative coordinates, clipping and out-of-range rejection.
- Build committed and pushed to both safety branches before EXE/DLL deployment.
- Installed captures use `.tmp-bulb/ocean.ps1`, private user profile, Nogova,
  1280x720, RTX 3070, default DLSS selection, 20-second runs. Every process exited 0.
- `ocean-far-before` (ea3c0494) / `ocean-far-after` (d243cae4): camera
  [60000,8000,8000], azimuth 90, elevation -65. The straight water boundary across
  the upper image disappears with continuation. Initial x=32000 capture did not
  clearly expose the boundary and is not the acceptance comparison.
- `ocean-negative-corner`: camera [-60000,-60000,8000], elevation -90. Continuous
  water beneath the camera beyond both negative map boundaries, no rectangular cut.
- `ocean-near-off` / `ocean-near-on`: same installed binary, camera
  [7900,4500,10], azimuth 0, elevation -8. Shoreline/near water preserved visually.
  Selected nodes 114 -> 143; LOD0-4 counts unchanged; only LOD5+ increases 10 -> 39.

Coordinates above use CLI x/north/altitude order. All logs/images are under
`.tmp-bulb/`. Foreign Cargo.lock SHA256 remains
9A4E74D791B26A4303E064867DA67B862C1439D0635DF3E19ED5A97AADF31A2E.

## Cost

Seven non-startup GPU samples per capture, medians in milliseconds:

| View | Water draw off / on | Water total off / on |
| --- | --- | --- |
| Normal coastal view | 1.541 / 1.745 | 3.427 / 3.585 |
| Far offshore aerial | 3.662 / 3.786 | 5.137 / 5.275 |

These short runs show roughly 0.15 ms additional total water GPU time in the
sampled views, not a statistically controlled benchmark or universal guarantee.
The new surface still incurs geometry and pixel cost where it fills previously
missing ocean. Wave simulation resolution/cascade count remains unchanged.
CPU selection is lazy and allocation-free apart from the existing selected-node
vector growth; no dedicated CPU timing claim is made. Very low flight offshore
can request fine water geometry just as low flight over the original core does.
