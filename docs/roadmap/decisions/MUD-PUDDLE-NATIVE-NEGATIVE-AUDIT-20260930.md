# Native shoreline negative control: source audit, 2026-09-30

## Finding

The fixed-UV return campaign `fixed-uv-return-20260930-123228-02e4e3`
failed only its negative changed-fraction gate: 4237 of 331776 central ROI
pixels changed by at least 8 channel levels (1.277%, limit 0.5%). The mean
0.363 remained below its independent noise-adjusted limit. Dirt and overhead
return masks had IoU 1; the master-off gate passed. Thresholds remain unchanged.

This camera is a mixed-material shoreline scene, not a source-pure BeachGrass
negative. Its camera is `(2756.25,2369.25,10.44,0,-18)`; the old fixture logger
named the **target** 12 metres north as BeachGrass, not the camera's land cell.
At X2756.25, cell centre Z2368.75 is Dirt_01 (0), Z2381.25 BeachGrass (16),
Z2393.75 Dirt_01 (0), Z2406.25 BeachGrass (16). Nearby cells mix these with rock.
The installed eligibility log correctly rejects material 16 despite its inherited
Dirt_01 BCR image. Its neighbours legitimately admit puddles on their Dirt cells.

Read-only pixel localisation distinguishes two effects:

* Master-off/dry changes: 2898 pixels, all in y144–253, visually sea/shore foam.
  Tidewater maintains prior-step state (detail drift and persistent foam); setting
  the same simulation time after startup does not reconstruct identical history.
* Wet/dry additionally has a 912-pixel patch at x474–645,y304–324 beneath the
  middle bush, shifting toward blue (mean RGB delta +1.25,+8.06,+24.92). Its
  exact ground coordinates were not read back. The source mixture means this
  cannot establish a BeachGrass admission defect. Below y330 there are zero
  changed pixels, mean absolute channel difference 0.003807.

Do not change production admission or relax the gate to fit these images.
Use a steep, height-queried view confined to an ineligible cell interior.

### CPU localisation of the blue patch

Intersecting camera rays with the actual native 6.25 metre bilinear heightfield
strongly supports neighbouring Dirt admission. Use camera X/Z 2756.25/2369.25,
Y 10.44, downward elevation 18 degrees, cLeft 0.933333 / cTop 0.525 and the recorded
1280x720 image. Convert pixel coordinates to normalized screen coordinates,
rotate ray `(screenX*cLeft,screenY*cTop,1)` by the recorded camera elevation,
then bisect its intersection with the source-derived native heightfield.

| Pixel X/Y | Estimated ground X/Z | Interpolated mud |
|---|---|---:|
| (510,313) | (2752.327,2389.368) | 0.999989 |
| (559,314) | (2753.816,2389.274) | 0.997871 |
| (474,304) | (2750.978,2390.513) | 0.855588 |
| (645,324) | (2756.393,2388.162) | 0.753901 |

The first ray resolves almost entirely to Dirt_01 cell `(220,191)`.
These values exceed the production 0.65 admission start. This is CPU geometric
inference, not a GPU depth/material readback, but it explains the observed small
blue film through legitimate Dirt source flags. No classifier change is warranted.

## Bounded stock-data audit

Read only `addons/data/worlds.pak` directory metadata, then three entries. No
whole terrain export, game, GPU or build was run. The archive FORM size, chunk
walk and directory walk close. Stored entry sizes and zlib outputs close.

| Entry | Bytes | SHA256 |
|---|---:|---|
| worlds/Eden/Eden/Eden.terr | 1545 | 8613ee6f503dcfd7fe796763c916c09686fdee898029b96fcb4cba4d9c118711 |
| worlds/Eden/Eden/.Data/Eden_0.ttile | 34298 | 9cd9e5abc643f416f7e6a9c53c13638030f4fcb721b42d27189906ff9535a03e |
| worlds/Eden/Eden/.Data/Eden_460.ttile | 52630 | fae6e04d5910d02be6a1485c40a813cab749c4a1caa21bc38b51dbe4cef5084c |

The descriptor says 6401x6401 height samples, 2 metre spacing, scale 1/32,
offset -204.78125, four 64 metre material subcells per tile edge. Tile0 HGHT
is 129x129: tiles span 256 metres, 50 tiles per world edge. Tile460 covers
X 2560–2816,Z 2304–2560. Its BMAT coordinates are **global subcell indices**,
not local tile indices. Decode each palette/blend using TerrainMaterials.hpp:
little-endian packed samples; depth-first quadrant order; every node and mask
block consumed exactly once. Do not replace the blend with palette entry 0.

Reproduce native `_tex` by sampling that decoded metre mask at the integer
part of each 12.5 metre land-cell centre, exactly as LandLoadEnfusion.cpp does.
The nearby cells are retained in the companion JSON. Native heights first
point-sample the 2 metre source into the 6.25 metre 2048 grid; GPU terrain height
then bilinearly samples these nodes. This audit used that order.

## Next candidate: confined BeachGrass interior

Centre `(2781.25,2381.25)` has material 16. The shader's `cc=world/12.5-0.5`
is exactly `(222,190)`, so its four taps are:

| Offset | Land cell | Material | Mud flag | Weight at centre |
|---|---|---:|---:|---:|
| 00 | (222,190) | 16 | 0 | 1 |
| 10 | (223,190) | 16 | 0 | 0 |
| 01 | (222,191) | 16 | 0 | 0 |
| 11 | (223,191) | 0 | 1 | 0 |

Its surrounding 3x3 rows (Z189..191, X221..223) are `[0,16,16]`,
`[16,16,16]`, `[0,16,0]`. This is deliberately **not** claimed to be a
uniformly ineligible 3x3 region. At the centre, interpolated mud is exactly 0.
With default blend_width 0.15, nearest-neighbour weights remain zero until
4.375 metres from the cell centre. The audit also sampled a 65x65 grid across
the +/-4 metre square: maximum mud 0. Native bilinear height at the centre
is 5.5625; this square's sampled height range is 5.5625–5.8025.

Use runtime `water_bathymetry` at this centre and add 5 metres, then pose
`2781.25 2381.25 <queried-height+5> 0 -85`. At the recorded 1280x720 view,
cTop 0.525 / cLeft 0.933, a central 60% ROI intersects approximately +/-2.9 metres
X and at most 2.1 metres forward on this height range. That lies inside the
zero-admission interior; the full viewport is not the acceptance region.
The runtime query and camera image still need verification for objects or
different settings. Existing numeric thresholds and master-off control apply.

This is a data-backed fixture candidate and diagnosis. It does not retrospectively
accept the failed shoreline gate, prove the unidentified bush patch's material,
or claim any new runtime appearance result.
