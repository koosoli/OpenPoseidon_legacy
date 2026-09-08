# Satellite far-texture assessment (2026-09-09)

Decision: retain the existing authored satellite path; defer an automatic classic
terrain colour baker. The owner authorized implementation only if worthwhile.
This is a scoped cost/benefit assessment, not a failed prototype benchmark.

## Existing implementation

TerrainWgpu.cpp binds authored satellite/mask/UV descriptors. In
rust/src/terrain/terrain.wgsl, sample_authored_cell samples that satellite in its
own TexGen frame. Beyond the detail fade, it returns satellite colour and the
optional tile normal before fetching the detail surface stack. This already
implements the relevant far-view technique for supported authored materials.

Classic CWA worlds use sample_cell with mip-filtered texture gradients and cell
blending instead. Uniform, unclamped neighbours take one ground colour fetch;
transition cells retain four. They do not have an automatically generated whole-
world satellite cache. A baker would need to preserve jitter, clamped transition
tiles, blend widths, colour-space rules and resource-generation invalidation.
It must not bake dynamic lighting, fog, shadows or moving objects into albedo.

Bohemia's terrain documentation confirms satellite imagery's far-view role:
https://community.bistudio.com/wiki/ArmA:_Terrain_Editing
This does not establish the quoted claim that every Arma generation automatically
creates a missing satellite at runtime. The separately found Workbench "SuperTexture"
page belongs to Carrier Command / Take On Mars, not proof of Reforger behaviour.

## Current evidence

Installed d243cae4, private profile, RTX 3070, 1280x720, default DLSS selection.
`.tmp-bulb/satmap-nogova-aerial-current.png/.log` was captured with ocean.ps1 at
CLI x/north/altitude [7764.82,4408.71,8000], azimuth 20, elevation -45, 20 seconds.
Process exit 0. Seven valid terrain GPU samples:

- Total median: 0.424 ms (range 0.378-0.434).
- Colour-pass median: 0.349 ms (range 0.307-0.380).

A satellite cache would replace only part of the colour-pass work, not terrain
geometry, normals or lighting. There is no measured cache speedup because no
prototype was implemented. The capture still shows coarse authored colour patches;
baking the same source colours cannot invent an artist-authored satellite image.
Filtering may reduce aliasing but could also blur coast/material transitions.
The thin rectangular offshore line visible in this aerial remains a separate
water/terrain artifact; the previous ocean continuation fixed finite ocean
coverage, not every seabed or reflection boundary.

For scale, a 4096-square RGBA8 cache with full mip chain is about 85.3 MiB before
compression; 8192-square is about 341.3 MiB. These are illustrative formats, not
mandatory allocations: compressed or paged designs can reduce residency, but add
baking, streaming and invalidation work. No such allocations were introduced.

Reconsider if higher-resolution/other-GPU profiling demonstrates a material
sampling bottleneck, or a bounded cache prototype demonstrates a clear visual
improvement with colour/shoreline parity. Current evidence does not justify
enabling an automatic generator. No renderer code or game settings changed.
