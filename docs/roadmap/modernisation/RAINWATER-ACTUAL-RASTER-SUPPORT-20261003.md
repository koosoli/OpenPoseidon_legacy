# Actual raster water support candidate

The reported floating puddle is not reproducible at present. This audit found
a specific source inconsistency; it does not reproduce that owner's scene.

Coarse water uses an exact native anti-diagonal bed plus bilinear depth, sampled
at the vertices of four subquads. The GPU linearly interpolates those vertices.
For depth mixed difference `delta=d00-d10-d01+d11`, the raster height minus the
analytic field head is `-delta*u*v` in the lower local triangle and
`-delta*(h-u)*(h-v)` in the upper, where `h=.5`. Maximum absolute error is
`abs(delta)/16`. The anti-diagonal native bed contributes zero error here.

Legitimate nonnegative checker depths `[0,D,D,0]` therefore rasterize above the
field by `D/8` at each subquad diagonal midpoint. A dense101x101 numerical
interpolation over an actual sloped anti-diagonal bed verifies the following:

| Corner peak D | Maximum current coarse raster error | 4x4 bound |8x8 bound |
|---|---|---|---|
|.02m|.0025m|.000625m|.00015625m|
|.08m|.01m|.0025m|.000625m|
|.24m|.03m|.0075m|.001875m|
|.5m|.0625m|.015625m|.00390625m|
|2m|.25m|.0625m|.015625m|

The simulation/publication has no maximum depth cap; the optical64m clamp is
only Beer-Lambert numerical range protection. More fixed tessellation increases
vertex count quadratically and cannot supply a universal3cm error bound without
a depth-curvature cap. Native fine heads are horizontal, so this error is zero.

The candidate adds a fragment support helper taking the actual interpolated
`in.world.y`. It rejects an absolute head disagreement over3cm and an actual
raster surface at/below the bare bed+1mm. The existing supported-depth, native
bed identity, sea and minimum liquid depth policy still applies. No water
position is forced, no mass changes and no public API changes occur.

Three centimetres is an explicit presentation approximation budget, not a
measured physical cutoff. Legitimate high-curvature coarse water can now be
clipped and produce gaps. This conservative fail-closed choice must not be
called a complete water geometry quality solution. Better native ownership or
bounded adaptive tessellation remains a separate task.

Required device probes use the actual production helper: analytically valid
.3m water accepts +/-3cm raster error and an exact horizontal head, rejects
both +/-25cm mismatches, rejects a raster surface under the real bed and retains
the sea negative. Existing complete actual vertex/fragment coarse/fine positive
and source/roof/snow/sea/underside negative fixtures remain unchanged. Device
execution and actual installed visual checks are root-owned and pending.
