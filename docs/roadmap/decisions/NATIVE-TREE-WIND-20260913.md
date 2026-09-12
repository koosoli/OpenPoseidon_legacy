# Native tree attachment wind

Baseline: asset-obs-001 / 212019e8; installed code before this change d7268a32.
Owner still observes separate Reforger trunk/crown movement. Earlier shared
phase and 2.5 cm flutter cap did not establish that this was fixed.

## Diagnosis

`frame.wgsl::veg_sway_offset` uses a nonlinear height power (default 1.6)
and extra translation for alpha-tested sections. XOB conversion has no imported
branch attachment weights. Alpha-tested geometry includes branch attachments,
not only independent leaf tips. Even identical phases do not make that extra
translation preserve an attachment to a solid trunk.

Separately, nonlinear per-vertex displacement does not commute with triangle
interpolation. A branch at height 12 between trunk vertices at 4 and 20 has
different displacement from the interpolated trunk edge. A numerical regression
fixture demonstrates this mechanism; it is NOT a measurement of a particular
authored tree's gap. Native capture `Forest-wind-attachment-before` contains eight
motion frames with 4x sway, native 1080p/MSAA4, DLSS off, LOD governor fixed to 1.
It completes normal shutdown. Capturing these frames invalidates FPS comparison.

## Bounded fix

Native `.xob` MapTree/MapSmallTree objects use a common affine horizontal shear
for all sections. The root at model Y=0 stays fixed. Native XOB conversion sets
autocenter=0 for every LOD, preserving that authored origin. The displacement is
linear in height, with the same per-object phase and live wind as before.
Stiffness changes its common slope (1.6 / stiffness), not separate vertex powers.

This is a deliberate fallback, not native Enfusion skeletal/branch simulation.
It trades flexible bending and unsupported leaf-only flutter for intact tree
attachments across material boundaries and different mesh densities. An imported
branch hierarchy with validated weights would be needed for independent branch
motion that preserves attachment. Do not infer those weights from unknown vertex
colour channels. Separate objects with different authored origins are not covered.

Direct path: Object.cpp publishes foliage kind 3; shader3d selects negative leaf
mode for solid AND cutout sections. Retained path: BuildGpuInstance adds bit 8;
gpu_driven selects the same mode. Existing flag lanes are reused; no buffer or
FFI layout grows. Direct temporal subset and both previous-frame shader formulas
use the same mode. Legacy P3D trees, native bushes and merged OFP forests retain
their existing animation and shading. No simulation or collision is changed.

Default enabled for those native trees. `WGR_NATIVE_COHERENT_WIND=0` before startup
restores the former motion for A/B diagnosis. No asset regeneration is needed.

## Acceptance

Code commit 8057395c built and installed; both safety branches pushed. BuildInfo
inside the binary says 212019e because generation preceded the commit; deployment
stamp and per-run provenance identify 8057395c. Matching EXE/DLL deployed together.

- Full PoseidonGame/PoseidonTests build passes.
- Native model classification: 7 C++ assertions pass (including legacy exclusion).
- Numerical attachment/interpolation and actual direct velocity-subset tests pass.
- Entry shader composition passes, including the standalone direct velocity shader.
- Same-build `Forest-wind-coherent-on` / `Forest-wind-coherent-off`: eight frames
  each, 4x sway, fixed camera, native 1920x1080/MSAA4, DLSS off, governor range 1.
  Both pass strict lifecycle, exit 0 and complete shutdown. Both retain 22,360
  registered / 6,463 main instances / 15,756,906 main triangles.
- `Forest-wind-normal-lods`: correction enabled by default (no wind-mode override),
  normal wind, governor range 16. Eight frames, clean lifecycle. The logged mix
  changes from 403/6,021 at LOD0/1 to 95/3,563/2,142 at LOD0/1/2 during capture.
  Final 5,800 main / 5,130,090 triangles is existing governor behaviour, NOT an
  optimization or same-quality comparison attributable to the wind change.

Inspected the motion samples for new disconnections, floating roots and gross
mesh deformation; none was apparent at the captured scale. The affine attachment
property is covered independently by the numerical test. This is acceptance of
the bounded fallback, not proof of native branch-animation fidelity on every
species. A continuous fast camera route and DLSS-enabled visual acceptance have
not been run for this change. The temporal mode is code/unit-test verified.
Shadows already use a separate undeformed caster path; this change does not claim
to resolve that existing gap.

Artifacts are under `build/native-reforger-perf/Forest-wind-*`. All motion runs
explicitly have performance_comparable=false: readbacks and changing wind/cloud
time preclude any FPS/tail-cost claim. No additional geometry, material maps,
per-instance buffer lanes or simulation work are introduced.

Native far-object coverage, forest/ground contrast and streaming tails remain
separate open tasks, not completed by this wind correction.
