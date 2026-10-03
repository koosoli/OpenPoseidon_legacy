# Sand contact normals: measured loss and bounded repair

Parent installed campaign:
`build/original-weather-player/wet-soil-sand-receipts-crlf-fixed-20261003-115204-1100e9`
on installed `b59f455a`, source `7fd0ac9a`. Original Eden ground is
`eden/ps.paa`, resolved Sand, sound `sand`. Ordinary player input created the
contacts; no diagnostic stamp was used. The CPU-to-Rust upload hashes passed,
but actual low/top PNGs did not show a readable depression. The neighboring mud
inspection does show depressions through the same terrain draw path.

## Measured source chain

The actual top-view little-endian payload is
`eden/sand-actual-contact-top.sand.f32`, 1,048,592 bytes. Its header is
`[5944,3292,0.125,0.15]`. At the inspected world point, sampling that uploaded
bilinear field reproduces the physical depth exactly. The renderer's minimum
normal stencil was a full cell on either side: a compulsory 25 cm span even
when a close pixel covered millimetres. This crosses the entire boot pit and
samples its other walls instead of the wall under the pixel.

| Actual world point X/Z | Uploaded depth | Physical bilinear derivative X/Z | Previous shader stencil X/Z |
| --- | --- | --- | --- |
| 5975.804199 / 3325.572998 | -0.09372286 m | -0.2190633 / +0.3146123 | +0.0265153 / -0.0270164 |
| Cell centre 5975.8125 / 3325.5625 | -0.09869628 m | +0.3512698 / +0.3005348 | +0.0750052 / -0.0803483 |

The first physical derivative agrees with the campaign's actual `dev_sand`
receipt. The second demonstrates the same loss at an exact source cell centre.
These are read-only numerical measurements of the captured payload, not an
executed GPU receipt. The fixed field's close shader derivative is the same
analytic derivative of those four bilinear source values.

The local CDLOD source path already refines to 4 m patches with a 32-quad mesh,
so nominal vertex spacing is 12.5 cm. That cell-corner lattice is half a cell
away from the source's cell-centred values: the captured field's roughly 10 cm
peak samples at about 6.47 cm on that mesh. This repair leaves that geometry
unchanged; it addresses the separately proven normal-sampling defect. Runtime
inspection must still establish whether other defects remain.

## Change and limits

Only `sand_height_gradient` changes, identically in terrain and shared conform
helpers. Resolved pixels at footprints up to 6.25 cm use the guarded bilinear
derivative. The existing height clamp and window-edge taper also differentiate,
so a disabled, incomplete or unsupported view cannot acquire a slope. Between
6.25 and 12.5 cm the slope smoothly converges to the previous filtered result.
At 12.5 cm and beyond the previous stencil is retained exactly. Distant filtering
does not sharpen, and heights, signed rims, uploaded data, physical contacts,
source admission, CDLOD selection and mud/snow gradients remain unchanged.

The resolved branch reads four source cells instead of the previous four
bilinear height samples. The short transition interval reads both forms. With
valid signed cells each component of the interior derivative is bounded by
`(0.15+0.02)/0.125 = 1.36`; the existing window taper adds at most 0.075.
Surface normals are normalized by the established caller.

## Verification

`Test-SandReliefShader.ps1 -DependencyDirectory <prebuilt debug/deps>` extracts
the actual CPU-only shader test and production composer, compiles against
existing Naga libraries with warnings treated as errors, and validates both
actual helper paths plus complete terrain colour/prepass composition. It passed
without creating a GPU device or launching a game.

The existing real-GPU sand geometry test now also samples resolved and
transition pixel footprints. Its four source depths define a bilinear slope
`[-0.16,-0.48]`; the previous stencil gave `[-0.04,-0.12]`, losing 75%.
The test requires the actual production shader to recover the physical slope
and its smooth halfway transition `[-0.10,-0.30]`. Existing signed depth/rim,
out-of-window, incomplete/disabled, Snow/Mud independence and terrain/conform
equivalence assertions remain. Those GPU assertions and installed low/top
images are pending the parent build/device owner; this source commit makes no
visual acceptance claim.
