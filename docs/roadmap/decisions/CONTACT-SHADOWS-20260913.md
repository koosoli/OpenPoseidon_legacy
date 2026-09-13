# Contact-hardening CSM prototype

Baseline: asset-obs-001, 7aea321f. Owner explicitly prioritized this shared
renderer experiment over native material work. Default remains fixed PCF.

## Implementation

Shared shaders/shadow.wgsl serves direct meshes, retained meshes, terrain and
grass. Existing alpha-tested caster generation is unchanged. A bounded blocker
search estimates mean blocker/receiver separation, converts normalized ortho
depth to metres using the cascade VP depth row, multiplies by tan(sun angular
radius), then converts metres to UV with each cascade's X/Y row lengths.
Camera distance and cascade index do not determine the physical radius.
Existing cascade coverage fallthrough and overlap blending remain intact.

Mode 0 preserves the fixed-filter branch. Mode 1 uses 17 blocker depth fetches
(including the centre) and 16 plane-corrected bilinear filter taps. Mode 2 uses
9 blocker fetches and 8 filter taps, on every cascade, not a softness-changing
far-tier switch. Contact/subtexel penumbrae use one bilinear comparison. The
sample disk is fixed, without frame noise or temporal rotation.

Dev tools / Shadows: contact mode 0/1/2 and sun angular radius in degrees.
Reset restores mode 0. Startup diagnostics: WGR_CONTACT_SHADOWS=0/1/2 and
WGR_SUN_ANGULAR_RADIUS (default 0.266 degrees, clamped 0..2). These use the
previously unused W lanes of camera-forward and sun-direction in the shared
shadow block; no FFI layout growth or new bindings. Local lamps are unaffected.

## Explicit prototype limits

- Search/penumbra estimate capped at 100 m light-direction separation, constant
  in world space. Extremely tall/distant casters can be missed by the search.
- Sparse blocker search and mean depth approximate multi-layer foliage; no
  guarantee of exact area-light visibility. No-blocker search retains the centre
  comparison rather than assuming thin cutouts are absent.
- Existing receiver bias and finite shadow resolution remain limitations.
- Stable sample orientation removes deliberate temporal noise, but does not
  prove absence of motion/cascade aliasing. Runtime acceptance is required.
- Not ray tracing, not native Reforger material repair, not terrain heightfield
  soft-shadow replacement. No new global-default quality claim.

## Validation

Eleven shader tests pass, including all receivers and coalesced-PCF composition.
New scalar test verifies metric radius invariance over several ortho widths,
depth ranges and caster separations. This is not visual acceptance.

Measure-ContactShadows.ps1 uses existing installed-game capture/lifecycle and
frame-trace mechanisms. Modes 0/1/2, native 1x, MSAA4, DLSS Quality; fixed exposure,
same scene, resolution and LOD settings. contact_shadows.eden places matching
Jeeps at 0.05/3/12 m and includes town buildings. Reference option reuses native
Reforger trees at the owner's camera. Runtime results are pending.

Receiver shading is fused into colour passes. Report matched differences in
receiver-containing pass timestamps as incremental receiver cost, not a fictitious
standalone absolute shadow-receiver timer. Report caster-pass GPU time separately
and full GPU frame time; capture readbacks are excluded from frame-trace statistics.
Repetition and motion/DLSS visual checks precede any recommendation/default change.
