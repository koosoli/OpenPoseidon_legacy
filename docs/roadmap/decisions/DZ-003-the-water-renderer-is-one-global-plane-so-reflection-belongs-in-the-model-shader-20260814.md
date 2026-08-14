# DZ-003 — The water renderer is one global plane, so DayZ's water reflects from the model shader instead

**Date:** 2026-08-14
**Status:** Decided, implemented and measured. DayZ water reflects the sky; it is still water, not
moving water.
**Follows:** [DZ-002](DZ-002-dayz-authors-river-flow-in-a-texture-not-the-placement-20260814.md).

## The question

DZ-002 established what DayZ water is: a near-black surface (`Color 0.0392 0.0549 0.0667`) whose entire
appearance comes from reflection — `EnvironmentMap`, `ReflectionColor 0.2 0.298 0 0`,
`FresnelRange 0.468`. Its material and textures now reach the renderer, and it draws as a flat dark
quad because nothing binds an environment map for model surfaces.

The engine already has a mature water renderer under `WgpuRenderer/rust/src/water/` — sky environment
mapping, planar reflection, screen-space reflected-scene tracing, refraction, FFT waves, foam. The
obvious move is to route DayZ's ponds and rivers into it and get all of that for free.

**That is the wrong move, and the reason is structural rather than a matter of effort.**

## The water renderer is a single global plane

There is no geometry submission API. `IWaterRenderer` has exactly one method —
`DrawWater(Scene&, int xBeg, int zBeg, int xEnd, int zEnd)` — cell-rectangle integers, no mesh, no
transform, no material. `WaterWgpu` builds one CDLOD quadtree over the whole map once, selects nodes
from it, and instances a shared 96×96 grid.

Height is **one scalar for the whole world**. `WgrWaterNode` carries no height field at all — its
`origin` is world XZ only — and every vertex is planted on the per-frame `sea_level` uniform, taken
from `Landscape::GetSeaLevel()`. That scalar is load-bearing in at least a dozen places: the vertex
plant, the LOD distance metric, the seabed clamp, the shoal depth, the planar-reflection plane point,
the surface-wave and foam reference, the whitewater passes, the underwater compositor, the FFT
spectrum UBOs, and the CPU screen-coverage estimator.

The codebase already says this, at exactly the place a per-body material would have gone:

> Optional directed surface flow… **The current CDLOD API has one global material, so river producers
> must not set this until per-water-body batches exist.**

and in `WaterWgpu.cpp`:

```cpp
// The sole draw path is the global ocean plane. Keep directed flow disabled until
// water-body batches can supply a river-only material signal.
```

There is also no *concept* of a bounded water body. Water existence is derived, not authored — the
quadtree comment is "water existence is defined by the terrain dipping below sea level", and
`Landscape::WaterDepth(x, z)` is a stub returning `0`. The OFP original is still visible as a
commented-out line: `// AutoArray<WaterLevel> _waters; // reflection levels in current scene`.

### What routing DayZ's water through it would actually cost

* Per-node height threaded through the vertex shader, fragment shader, foam, whitewater, the FFT
  spectrum UBOs and the underwater compositor — most of which read `sea_level` as a UBO scalar, not a
  per-instance value.
* A second geometry source. `SelectVisibleCdlod` prunes any node whose terrain sits above
  `maxTide + maxWave + 1 m`, so **a pond at 170 m over terrain at ~170 m is pruned by construction**.
* The seabed clamp is written on the assumption of a global plane over a heightmap
  (`floor_y = min(seabed_y + 0.02, sea_level)`), with a comment explaining that the cap exists so the
  ocean grid stays at sea level over dry land. A pond at 170 m breaks that in both directions.
* Every depth-driven term — body tint, coast fade, foam width, shore factor, breakers — is calibrated
  on `sea_level − seabed`. A pond whose terrain is also near 170 m gives depth ≈ 0 everywhere, which
  lands in the shoreline-foam band: **the whole pond would render as surf.**
* One planar reflection plane per frame: one reflected camera, one clip plane, one target. Serving the
  sea at ~0 m *and* ponds at 170 m needs two passes minimum.
* 804 river segments are a branching network of arbitrary polygons, not a quadtree. Snapping them onto
  axis-aligned square nodes loses the channel outline, which is the entire point of a river.

## The decision

Add a Fresnel-weighted environment reflection to the **ordinary model shader**, for materials that
declare one. DayZ water is a flat quad with a near-black base and a Fresnel-weighted environment
term — that is precisely what this implements, and it needs no FFT, no foam, no refraction, no planar
reflection and no water-body concept.

Three things make it cheap:

1. **The environment map already exists.** `sky::env_view()` is handed to water today and sampled as
   equirectangular linear radiance; `gfx3d` binds nothing of the sort. DayZ's own env map
   (`Enfusion/Graphics/Textures/Water/env_lake_co.edds`, 256×256) is the **same equirect projection**
   — verified by converting and looking at it — so the sampling convention is one the codebase
   already has.
2. **No ABI struct has to grow.** `WgrModelMaterial` is exactly 192 bytes with `static_assert`s on
   both sides and an FNV-1a size hash, and it has no slack. It does not need any: there are four free
   lanes already in place — `WgrModelSection::_pad`, spare bits in `WgrDraw3D::flags`,
   `SectionMaterialGpu::_pad_layer[3]` and `MaterialUbo.normal_map.w`. A reflective flag rides in
   those with no size change and no hash change.
3. **It lands where nobody is working.** `rust/src/gfx3d/**`, `rust/src/sky/**`, `rust/src/water/**`
   and `lib.rs` are clean; the concurrent agent's uncommitted work is in `EngineWgpu.{cpp,hpp}`,
   `include/wgpu_renderer.hpp`, `ffi.rs` and `godrays.rs`. Routing into the water renderer is the
   mirror image: it lands squarely in `water/` and `lib.rs`.

The new binding goes in **gfx3d's group(1)**, not `frame.wgsl` group(0) — group(0) is shared with
terrain, grass and water, and a binding there has a far wider blast radius.

## How much of the game this reaches, and the trap in the trigger condition

Measured over both shipped worlds' placements:

| world | addon | placements |
|---|---|---|
| ChernarusPlus | `dz\water\` | 670 ponds + 3,374 streams/beds = **4,044** |
| Enoch (Livonia) | `dz\water_bliss\` | 434 ponds + 804 river segments = **1,238** |
| | | **5,282 placed water surfaces** |

The two worlds use *different* water addons, and that difference nearly produced a fix that
covered the smaller map only.

**`water.pbo` contains zero `.emat` files** — 47 rvmats, no `.emat` at all. And Chernarus's
`ponds/data/water_lake.rvmat` is **byte-for-byte identical** to Enoch's pond stub: the same 79-byte
rapified file whose entire content is `PixelShaderID="CalmWater"; VertexShaderID="CalmWater"`.

So `CalmWater` is **a shader family the engine is expected to implement**, and the `.emat` is a newer,
optional parameter override that only the `water_bliss` addon ships. Enoch's water is parameterised;
Chernarus's water is the same surface with no parameters at all.

The consequence for the implementation is concrete: **the reflective term must key on the shader
family, not on the presence of an environment slot.** Keying on the slot — the obvious choice, and the
one first specified — would have given reflection to Livonia's 1,238 surfaces and nothing to
Chernarus's 4,044, on the main map, with no error anywhere to say so. Where the `.emat` exists its
parameters are used; where it does not, the family still identifies the surface as water and built-in
constants apply.

`EmatMaterialAdapter::TranslateEmat` keeps the rvmat's own family string rather than inventing one
(DZ-002 §4), which is what makes the family usable as the trigger after substitution.

## Result

Implemented and captured on Enoch, same camera, same binary, reflection toggled by env var so the
comparison cannot drift between builds:

| region | before | after |
|---|---|---|
| **pond** (149,514 changed px) | (0.53, 0.98, 1.97) | **(24.00, 25.53, 31.72)** |
| control — brick wall | (21.76, 24.25, 29.74) | (21.83, 24.26, 29.65) |
| control — rock ridge | (9.16, 10.44, 13.72) | (9.19, 10.45, 13.67) |
| control — rocks right | (4.49, 5.04, 6.04) | (4.51, 5.05, 6.02) |
| control — black sky | (1.07, 0.99, 0.34) | (1.08, 1.00, 0.34) |

The change is confined to a single bounding box covering the pond quad — 10.4% of the frame. Every
control sits inside run-to-run noise (two runs of the same build differ by <= 2/255). **Zero
uncaptured wgpu errors**, no ABI struct changed size, and the unit suite fails exactly the 11 tests
it already failed.

DayZ water uses the **retained GPU-driven path** — confirmed on `enoch_50x50.p3d`, section 0, a
2-face 50x50 m quad — so that is the path that matters; the per-draw path is implemented but is not
what these surfaces take.

### The trigger, settled by measurement rather than argument

The plan allowed keying the reflective bit on the shader family *and, optionally, also* on the
material having an environment slot. Measurement removed the second option outright: in one Enoch
view, **157 ordinary `Super` materials** — boulders, tree stumps, anthills — name
`dz\data\data\env_land_co.paa`. With the slot as a trigger, control rocks moved **+21 to +37 per
channel** and the frame became a hall of mirrors. An environment *stage* means "this material has an
environment texture", not "this surface is reflective". Family only.

### Two corrections this produced

* **`MaterialUbo.normal_map.w` was not a free lane.** The plan listed it as unused; it already
  carried the material-debug invert-normal-Y flag. It is now a 2-bit field (bit 0 invert-Y, bit 1
  reflective) rather than one flag silently clobbering another.
* **The pond's own authored env map does not load.** `enfusion\graphics	extures\water\env_lake_co.edds`
  is an Enfusion engine path outside the `dz\` addon tree, so the resolver never finds it. It does
  not matter here — the live sky is reflected instead, and it is a better source — but it explains the
  `authored env map absent` line in the log, and it means DayZ's `ReflectionColor`/`FresnelRange` have
  no bound texture to accompany them.

### What it looks like

Still water. The pond went from a flat near-black hole to a surface carrying the sky, brighter toward
the grazing edge, which is Fresnel behaving correctly. It is **glassy**: the normal map is bound and
feeds the shading normal but produces no visible perturbation at this scale, and nothing animates it.
That is the expected state — flow was always sequenced after reflection, and now there is finally
something lit for flow to animate.

## What this decision does not do

* **It is not a water renderer for DayZ.** No waves, no foam, no refraction, no depth scattering, no
  caustics. Every parameter for those is parsed and sitting in the `EmatMaterial` unread (DZ-002 §2).
* **It does not make rivers flow.** Flow is fully decoded and its indexing and axis convention settled
  (DZ-002 §1). It was deliberately sequenced after this, and is now the next step.
* **It does not carry DayZ's own reflection parameters.** `ReflectionColor` and `FresnelRange` are
  parsed but not reaching the GPU — there is no free ABI lane, and Chernarus authors neither. Both
  worlds use a built-in `F0 = 0.02`, commented as the stated approximation it is.
* **Chernarus is not captured.** Its 4,044 placements work *by construction* through the family
  trigger, but only Enoch has been photographed.
* **It does not give DayZ water a per-body reflection of its actual surroundings** — it reflects the
  engine's sky environment, which is the right first approximation for a horizontal surface and is
  what DayZ's own baked env map approximates too.
* **Per-water-body support in the real water renderer remains the eventual answer** for anything
  needing waves or refraction on inland water. This record is the case for not attempting it now, not
  a claim that it is unnecessary.

## Two things left unverified

* **Enoch's runtime sea level.** The path is `_seaLevel = maxTide * tide`, but where DayZ's WRP
  supplies tide was not found, so the numeric gap between the ocean plane and the 170 m ponds is
  unmeasured. It does not change the analysis — the pruning test and the seabed clamp fail for *any*
  pond height differing from the global one — but the number is not established.
* **Which draw path DayZ water takes.** `ClassifyGpuSection` decides per section between the retained
  GPU-driven path and per-draw `DrawSectionTL`, and its verdict for a 2-face `CalmWater` quad was not
  traced. The implementation covers both for that reason.
