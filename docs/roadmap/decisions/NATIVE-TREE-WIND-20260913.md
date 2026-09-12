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

Initial tests pass: numerical attachment/interpolation fixture, actual direct
velocity-subset classification, and composition of object/velocity shaders.
Full build, native model classification unit test, installed A/B motion captures
and normal-wind/LOD coverage are pending at this checkpoint. Do not yet report
the user's complete visual issue as verified fixed. Shadows already use a separate
undeformed caster path; this change does not claim to resolve that existing gap.

Native far-object coverage, forest/ground contrast and streaming tails remain
separate open tasks, not completed by this wind correction.
