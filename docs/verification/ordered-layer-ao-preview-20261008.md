# Optional ordered per-layer AO

`WGR_REFORGER_AO_PREVIEW=1` applies finite nonnegative `AO_1..4` multipliers to
the corresponding original NMO alpha channels. An absent multiplier is 1.
Each scaled alpha clamps to [0,1] before the existing ordered composition.
Clamping after mixing is different: with half RGB masks, uniform alpha .8 and
scales (1,1,2,1), the correct result is .85, whereas post-mix clamping yields 1.

The option is off by default and requires both PBR and ordered-layer previews.
`WGR_REFORGER_AO_MATERIAL` optionally limits it to a case-insensitive path suffix.
A fresh process is required. Admission uses the existing supported four-layer
normal source contract and complete uploaded original colour/normal bindings;
unsupported or incomplete materials keep the previous AO path. It does not
require the normal or authored-tangent preview to be enabled.

ABI 24 and all struct sizes/offsets remain unchanged. Section flag 2048 assigns
the existing otherwise unused per-layer tint alpha lanes to AO multipliers only
for admitted sections. RGB tint stays intact. No extra texture fetches,
descriptors, backing allocations, vertex attributes, varyings or passes are added.
Only retained native PBR composition consumes the new flag. Legacy and direct
drawing retain their previous behavior.

Validation covers source defaults, zero and greater-than-one values, invalid
scalars and unsupported contracts; actual C++ production/test syntax; whole
Rust test metadata; production retained/direct/shadow shader composition; and
production-helper validation. A separate synthetic GPU readback test checks
identity, zero, saturation, overlapping masks and later-layer replacement.
The integrator runs the GPU test and installed fixed-camera A/B.

Hold camera, lighting, exposure, PBR, order, sRGB, normal/tangent settings and
material filter fixed; change only the AO switch. Require the named material's
`Wgpu native PBR layer AO preview` entry to report successful admission and the
intended four scales. Keep the option off until visual acceptance. This corrects
per-layer AO arithmetic only: global AO/overlay, weather, BRDF and lighting
remain separate limitations of the preview.
