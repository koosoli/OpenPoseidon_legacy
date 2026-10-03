# Broad wet-textile sheen candidate

Stock wet uniforms previously changed absorption only. The exact actual-person
wetness and filtered cloth UV mask now also feed a shader-local `ShadeMaterial`
value. The shared object shader adds a restrained rough dielectric sheen after
the existing lighting. It uses the interpolated geometric cloth normal rather
than authored fine normal-map grain or albedo contrast, preserving model creases
without producing moving bright texture speckles.

Existing CPU admission is unchanged: actual Person, primary owner LOD identity,
matching admitted clothing material across combined sections, exact stock atlas
or separate cloth identity. Existing West/East atlas exclusions still protect
hands, skin, boots, helmet and baked whole-body far LOD islands. The same decoder
and derivative-filtered mask drive absorption and sheen. Negative muzzle-flash
markers and exact dry zero retain their old outputs. No public material buffer,
instance layout, FFI, weather simulation or new source admission is introduced.

Direct plain and GPU-skinned draws share the shader. Retained static world draws
explicitly initialize wet cloth to zero: their shared section materials have no
per-person wet history and must not be wetted by global rainfall.

The layer uses water-interface F0 of 0.02 with a rough-surface grazing ceiling of
0.12. The normalized broad Blinn lobe exponent eases from 16 to 8 as the actual
world-space pixel footprint grows, giving a broad highlight without time/position
noise, SSR or mirror reflections. These are bounded appearance parameters for
wet fibres, not a calibrated woven-cloth BRDF or measured microrelief model.

The outgoing reflected fraction is removed from the underlying lit colour.
Sky reflection uses the same actual directional low-frequency sky irradiance,
ambient occlusion, interior/baked visibility and cavity factors as object lighting.
Sun response uses actual physical sun radiance and existing cloud/CSM/terrain
visibility. Local reflection reuses the actual bounded point/spot attenuation
and spot/cube shadow implementation. A wet coat therefore stays visible only
under light that actually reaches it. Existing retained wetness remains visible
after rain stops and under a roof; existing physical shelter probes gate filling,
not an immediate shader disappearance.

Only actual wet HDR cloth pays a second bounded local-light evaluation. No CPU
queries or additional GPU resources are added. Dry/noncloth and retained static
paths skip the branch exactly. The extra local-light/shadow cost on wet soldiers
under many lamps still requires root performance verification. Legacy LDR keeps
its existing absorption response without the new sheen.

## Verification

`Test-WetTextileShader.ps1` passed on 2026-10-03 using existing matching naga and
naga_oil dependency libraries. It extracts the actual production composer and
focused CPU test declarations, validates actual cloth/response helpers, checks
masked direct admission and explicit dry retained material initialization, then
composes the complete direct and retained entrypoint shaders. It does not create
a GPU device or launch a game.

The existing device UV/linear-hue/skin tests remain intact. A new actual-helper
device probe checks finite outputs, exact dry/refusal zero, retained dampness,
the 0.02–0.12 Fresnel bounds, 8–16 lobe bounds, nonnegative peak coefficient below
0.015, zero backlit direct response, and no energy gain on a unit white substrate
under a uniform unit sky. This is a bounded analytical-lobe/ambient-reallocation
check, not a general rendered-scene or integrated-BRDF energy proof.

Full combined renderer tests/build, installed dry/wet/covered/day/night cloth
captures, skin/proxy negative views and visual/performance acceptance are pending
with root. No candidate game or device run was performed by this source agent.
