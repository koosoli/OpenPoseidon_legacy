# Retained rainwater: remaining optical differences

Read-only source audit at `94f261c57ce441f4d3dc1f85030e83c75003dfaf`.
No renderer, forcing, geometry, simulation or material changes accompany this
document. The installed 90 mm/h rain calibration and its visual judgement are
pending. The existing installed-game receipt at
`build/original-weather-player/actual-water-depth-and-bare-soil-20261003-161012-8194fd/result.json`
was read directly; no new runtime was launched here.

## Existing improvements remain applicable

`rain_water.wgsl` already uses the world-fixed, independently timed/jittered
rain impacts in `shaders/rain_water_optics.wgsl`, actual opaque-copy SSR in
`rain_water_reflections.wgsl`, and a separately shadowed dielectric GGX solar
lobe. Direct body light shares the actual CSM/terrain/cloud visibility. The
native bed, physical depth, sea, source identity, raster support, roof, snow,
front-face and ownership guards remain necessary. There is no reason to repeat
these changes or brighten the whole reflection/body to hide a depth problem.

The impact helper fades above a 0.16 m pixel footprint. Each event has an
independent 1.4–2.7 s period, 0.55–1.05 s lifetime and changing centre; the
old simultaneous regular rain rings have already been removed. The separate
flow normal still uses two sinusoidal coordinates advected by the current
physical velocity times absolute time. Changing velocity can change that phase
abruptly. An actual near-camera motion sequence and successive flow values are
needed before calling this a visible defect or changing its animation.

## A single optical medium currently covers every substrate

`WgrRainWaterParams` and the coarse float4 cells carry domain/time/rain and
head/depth/flow. `WgrRainWaterFineCell` adds native geometry and ownership, not
soil identity or suspended sediment. `RainWaterPublication.hpp` packs exactly
those fields. The renderer's `rain_water_bare_terrain_layout` binds only the
bare heightmap and its sampling parameters. No terrain material table or source
index map reaches the retained-water fragment.

Consequently `rain_water_body_radiance` uses the same low reflectance
`(0.045, 0.060, 0.052)` everywhere and `rain_water_surface_optics` uses scalar
transmission `exp(-3 * actualLocalDepth)`. Real mud below a pool cannot currently
make its water medium different from water above sand or hard ground.

The receipt's
`maps.eden.gates.mudLighting.views.contact-clear-low.fragmentDiagnostic.waterReceipt`
confirms the primary `[6425.8042, 7175.5742]` source as actual
`eden\\pol.paa`, `Field/files=pol/sound=dirt/character=""`, eligible, with
42.370986 mm coarse water depth, bare bed 141.8849875 m and signed mud offset
-0.1759574 m. The primary head is 141.9273682 m. Eligible neighbouring Field
points reach 78.471638 mm; actual Grass neighbours are refused by the soft-source
classifier despite holding 16.547600–41.501991 mm of water. A global muddy pool
profile would therefore change genuine non-mud ground in this actual scene.
Fine is OFF in this receipt, and these CPU values are neither fragment-depth nor
GPU-submission proof. The signed impression is not additional physical rainwater
depth: retain the existing bare-bed convention in this audit.

The following are direct evaluations of the current source, with complete
coverage, no fog and a vertical view. Weights multiply the corresponding
radiances; direct solar reflection is separate.

| Actual depth | Reflection | Body | Transmitted bed |
| --- | ---: | ---: | ---: |
| 42 mm | 2.26% | 11.57% | 86.17% |
| 50 mm | 2.50% | 13.58% | 83.92% |
| 78 mm | 2.50% | 20.34% | 77.16% |

At view cosine 0.2, reflection becomes 31.18–34.45%. A grazing flat pool can
therefore mirror strongly while its overhead view primarily shows the bed.
The 8–50 mm rough-film/standing-water transition also matters: lowering rainfall
may reduce retained depth and change this response without changing any optical
coefficient. These arithmetic evaluations establish neither picture quality nor
a causal explanation of the user's remaining complaint.

## What Tidewater supplies, and what it does not

The active comparison is `water_tw/tw_water.wgsl`, not the separate classic
`water/water.wgsl` backend.

* Both paths use a baked atmospheric sky and a separate GGX sun. Tidewater's
  base GGX alpha is 0.035; retained water's is 0.08. Tidewater adds unresolved
  FFT/wind slope variance, foam and aeration. Retained water adds derivative
  normal variation. Simply making shallow water's lobe narrower would risk
  restoring distant glints.
* Tidewater adjusts the sky direction/horizon coverage using unresolved slope
  variance. Retained water samples the reflected direction directly. This is a
  possible narrow later investigation if an actual grazing capture shows a
  bad horizon response; it does not justify a camera-dependent Fresnel gain.
* Both SSR implementations fetch opaque colour at LOD 0. Retained water fades
  SSR confidence with normal variation; it does not blur valid reflections.
  Tidewater's 11 steps/3 refinements and much longer, more permissive ray gate
  do not supply such a blur either. Retained water's 24 steps/4 refinements,
  25 m limit and tight world-position/local-plane checks protect against false
  self/bed and screen-discontinuity reflections. Preserve those checks.
* Tidewater computes refracted optical path length, RGB absorption/scattering,
  and attenuates light reaching the bed. Retained water uses vertical depth and
  scalar destination transmission. A horizontal interface's Snell path factor
  is bounded to about 1.51 for IOR 1.333, so correcting that alone is modest and
  cannot create source-specific muddy water.
* `tw_shore.wgsl::shoreSurfMedium` uses actual shore depth/exposure and enabled
  breaker state; the water fragment further uses shoreline simulation residue
  and aeration. These are coastal sediment sources, not original OFP soil
  metadata. Copying the surf response onto inland ground would invent them.
* Tidewater's accepted default `GLOW_DEFAULT` is zero: body in-scattering and
  crest subsurface light are disabled in favour of reflection plus the
  attenuated bed. Copying its luminous surf body would contradict that default
  and could bring the reported milky appearance back.

Existing optical helpers and original material albedos are the relevant assets;
this problem does not require a generated texture. Classic water's
`water_optics.wgsl` provides separate absorption/scattering/energy helpers, but
its authored deep swatch and water-body profile are not automatically attached
to rainwater or to the Tidewater backend.

## Smallest defensible next slice

First judge the installed calibrated rain campaign with recorded actual depths,
camera, sky and ground source. If a genuine retained pool over admitted mud still
reads as clear water, add a **render-only, source-qualified optical medium**:

1. Reuse actual terrain source classification/indexing. `TerrainWgpu.cpp`
   already exports conservative legacy/native soft-soil bits and authored
   soft-slot bits in `WgrTerrainMaterial::puddle_flags`. Terrain's
   `source_mud_fraction` includes unknown selected slots in its denominator;
   do not promote a mixed or unbound source to complete mud. Stock cultivated
   soil and exact audited cultivated assets retain their existing guards.
2. Bind the necessary source lookup to rainwater with the same world/generation
   and source lifetime as the existing terrain resources. Do not repurpose
   physical volume, rain intensity, flow, geometry or the fine depth lane as
   turbidity. For a small first implementation, genuine pure legacy/native soil
   can be supported while authored mixed masks remain explicitly unchanged
   until their exact coverage is available.
3. Introduce bounded material optical parameters with current clear-water values
   as the zero/unknown/hard/sand case. A soil flag establishes substrate identity;
   it does **not** establish suspended sediment concentration, erosion, sediment
   mass or transport. Describe any first soil profile as an appearance model.
   Avoid a global brown colour or a new luminous milk-like body.
4. Preserve energy allocation. Current `ALPHA_BLENDING` transmits the destination
   by one scalar. True RGB extinction requires either a separately justified
   composition change or a validated read of the existing opaque colour/depth
   copy for the actual bed. A muddy body tint alone is not RGB absorption.
   Decide this contract before implementing a stronger coloured extinction.

### Existing-resource transport, without another field scan

`terrain/mod.rs::Terrain` already owns `index_map`, `materials` and `params_ubo`.
They are uploaded through the existing terrain API; their fields are private
Rust resources, not missing C++ data. Rainwater group 3 is currently empty. A
small group-3 layout plus a Terrain accessor/bind constructor could borrow these
three resources without growing the public ABI or uploading a second semantic
texture. Access is bounded by land-map dimensions, `CELL_LAYER_MASK`, storage
row count and current terrain/source readiness. Out-of-domain or invalid rows
must yield the unchanged clear medium, rather than clamp a distant point into
an eligible map-edge cell.

The narrow first shader query can use the actual unjittered world land-cell
index, pure legacy/native `puddle_flags & 2`, and the already shared
`ground_wet::cultivated_soil_fraction` bit-4 central-cell gate. Refuse authored
multi-surface rows until their actual mask coverage is available; an "any soft
slot" bit is insufficient. Do not interpolate soil identity across a hard,
sand or unknown cell or derive it from ground RGB. Original Cain's unauthored
Default sources consequently remain unclassified; rainfall does not authorize
reclassifying them. Boundary appearance must be observed separately rather than
smoothed by promoting neighbouring sources.

Move the exact WGSL TerrainMaterial row declaration into a shared import if
implemented, instead of creating a second independently maintained 384-byte
mirror. Cache binds by an internal **material/index resource generation**:
`set_index_map` and `set_materials` replace their resources and rebuild terrain
group 2, but do not increment current `heightmap_gen`/`mask_gen`. Reusing only
the bare-height generation would leave a stale optical-source bind. World
replacement must invalidate readiness until both the current map and material
table are present. This is an internal Rust lifetime change; it need not add
public ABI fields or perform any per-frame all-grid `SurfaceAt` scan.

Required counterchecks include unchanged unknown/hard/sand/zero-soil output,
mixed-source nonpromotion, stale source rejection, zero-depth/film continuity,
nonnegative finite energy, direct-light shadow/night behaviour, unchanged
support/sea/roof/snow admission, and actual original-map low/top/mid captures.
Keep hydrological budgets and geometry byte-identical. Actual moving-rain and
drying observations remain required; source arithmetic cannot accept them.
