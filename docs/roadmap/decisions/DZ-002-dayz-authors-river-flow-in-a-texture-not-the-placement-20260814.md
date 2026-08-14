# DZ-002 — DayZ authors river flow in a texture, and its water materials live in a container the engine could not open

**Date:** 2026-08-14
**Status:** Measured, and implemented. DayZ water now loads its real material and textures and no
longer renders neon green. It does not yet *shade* as water: DayZ authors water almost black and
gets its appearance from reflection, which no backend binds yet — see §4.
**Corpus:** the owner's retail DayZ install, `D:\SteamLibrary\steamapps\common\DayZ` — `Addons`, `dta`, and
`sakhal\Addons`. 553 DDS-family textures (454 `.edds` + 99 plain `.dds`) and 207 `.emat`.
**Follows:** [DZ-001](DZ-001-dayz-is-an-arma-2-descendant-20260814.md).

**Licence note.** DayZ data is proprietary — it is not APL-SA like the Arma corpora. Nothing from it is
vendored here, no fixture is committed, and every byte read during this work lived in a session
scratchpad. Both new test suites build their inputs byte by byte from the format rather than loading a
fixture, for exactly this reason.

---

## The claim

Three things, in the order they were found:

1. **River flow direction *is* authored.** The [handover](../HANDOVER-20260813.md) recorded that it was
   not, and proposed deriving it from each river segment's placement basis. That proposal is **refuted
   by measurement** — every one of the 804 placements carries the identity basis. The real source is a
   texture named by the material: `WaterStreamMap`, a 2048×2 unit-vector field.
2. **The material the engine reads is the wrong file.** A DayZ water surface carries two materials side
   by side. The `.rvmat` is a 79-byte stub with no stages; the `.emat` sibling holds everything.
3. **The `.emat`'s textures were unloadable**, because `.edds` is not the plain DDS it looks like, and
   neither `.edds` nor `.dds` was registered with the texture-source selector at all.

Each is now measured to closure, and the asset layer for all three is implemented and tested.

---

## 1. Flow direction: the handover's hypothesis, tested and refuted

The handover proposed: *"each river segment is a `Oprw25Object` with a full `Matrix4x3` basis (rows are
aside/up/dir/pos), so the segment's `dir` row is its downstream axis"*, and — correctly — flagged it as
an inference to verify before building on.

Verified, and it does not hold. Dumping every `dz\water_bliss\river\*` placement out of `enoch.wrp`:

```
segments                       804
identity basis                 804 / 804  (100.0%)
  aside 1,0,0   up 0,1,0   dir 0,0,1
Y                              169.6 .. 170.0  (3 distinct values)
```

Every river segment mesh is baked in world space and placed with no rotation, so the transform carries
no direction at all. Reading `dir` would have given every river on Enoch the same due-north flow.

**What does carry it** is in the `.emat`, and it is unambiguous — `DZ\water_bliss\river\data\enoch_river.emat`:

```
WaterStreamMap    "{EF177E7C8B896505}DZ/water_bliss/river/data/enoch_river_flowmap_co.edds"
StreamSpeedInfl   0.15
StreamNormalPower 2
```

The handover's "the `.emat` has `WindInfl` but no flow vector" was read off a **pond** material. Ponds
genuinely author no stream keys (they use `TimeMul` instead) — which is how the two surface kinds are
told apart. Decoding the flow map confirms it is a direction field and not, say, a tint:

| | |
|---|---|
| Dimensions / format | 2048 × 2, BGRX8, 12 mip levels |
| R,G as `2v-1` | 1,870 of 2,048 texels within 6% of unit length |
| B | ≤ 17, mean 0.0 |
| A | 255 everywhere |
| Rows | the two rows are identical |

So: **R and G are a unit flow vector, B and A carry nothing, and the field is one-dimensional.**

### How the mesh addresses that strip — measured, and it closes

The river meshes carry exactly **one** UV set, and its two coordinates are on wildly different scales.
Per segment, u spans a median of **0.002** while v spans **0.531**. That is the tell: **u is a global
parameter along the river network** and v is a local coordinate that tiles.

Two independent files then agree that u is the flow-map index:

* Across all 804 meshes, u never exceeds **0.913**. `0.913 × 2048 = 1869.8`.
* In the flow map, texels **0..1869 are 1870/1870 unit-length** (mean magnitude 1.002) and texels
  **1870..2047 are 0/178** — they are `(0,0)` padding. The last authored texel is index **1869**.

The mesh UVs and the texture's populated range agree to within one texel, derived from files that share
nothing. The texture is 2048 wide only because that is the next power of two above 1870.

**The vector convention is settled too, and it is the plain one.** For each segment, fit u as a linear
function of world (x, z) over its vertices; the gradient gives the world-space direction in which u
increases. Compare that against the flow map sampled at the segment's own u:

| `(R,G)` read as | mean dot | aligned > 0.7 |
|---|---|---|
| **world (X, Z)** | **+0.996** | **802 / 804 (99.8%)** |
| world (X, −Z) | +0.327 | 313 / 804 |
| world (Z, X) | +0.287 | 342 / 804 |
| world (−X, −Z) | −0.996 | 0 / 804 |

So `2·(R,G) − 1` is a **world-space XZ unit vector, no axis swap and no sign flip**, and it points along
increasing u. The two stragglers are both 3-vertex meshes — a single triangle, where the plane fit has
no residual freedom and a sliver gives an unreliable gradient. Both still sample a unit-length flow
vector and both dots are still positive, so **804 of 804 agree in sign**; only the two triangles fall
outside 45°.

Scale, from the same fit: the median |∇u| is 4.82e−05 per metre, i.e. **20,740 m per unit u** and
**10.1 m per flow-map texel**. 1,870 authored texels at ~10 m is roughly 19 km of river on a 12.8 km
map, which is the right order for a branching network.

### u runs along the river and v runs across it

An earlier draft of this document said v was the along-flow coordinate and told an implementer to
scroll the normal map along it. **That was wrong**, asserted from one segment and never measured.
Fitting both gradients over all 804 meshes:

| ∇u vs ∇v | |
|---|---|
| angle, p05 / p50 / p95 | 81.7° / **88.8°** / 89.9° |
| within 10° of perpendicular | **774 / 804 (96.3%)** |
| within 10° of parallel | **0 / 804** |

So v is **across** the channel, at ~57.6 m per unit v, while u runs along it at 20,740 m per unit.
On a 1024-wide normal map that is roughly 1.7 texels per 50 m along the flow against ~890 across it:
the normal map is deliberately stretched along the flow axis, which is what gives moving water its
streaks. It is good authoring, and it is the opposite of what the earlier draft claimed.

This also rules out the cheap version of the animation. Scrolling u would be "along the flow", but
u is a *network-wide* parameter — advancing it does not move water downstream, it slides the sample
along the whole river system. Animating this properly needs the world-space flow vector in the
shader, which is exactly what the flow map provides:

1. sample `WaterStreamMap` at the mesh's **u** — v is irrelevant, the texture is 2 rows and they are identical;
2. decode `2·(R,G) − 1` as a world XZ direction;
3. offset the normal-map sample along that direction over time, scaled by `StreamSpeedInfl` (0.15),
   and weight the perturbation by `StreamNormalPower` (2).

### What the geometry still cannot tell us

* **Which end is downstream.** Every river vertex sits within ±0.03 m of its placement height, and every
  placement is at world Y ≈ 170 — the whole network is a flat plane. There is no downhill to check the
  sign against. The flow map's sign is globally self-consistent and it is what the game itself uses, so
  following it reproduces DayZ's own appearance; calling it "downstream" is the author's claim, not a
  measured one.
* **Segment index order is a weaker signal than the UV gradient, and should not be used.** Consecutive
  indices are usually adjacent (p50 gap 49.4 m; 761 of 803 within 100 m, the ~42 exceptions being
  tributary boundaries) — but chaining them and comparing against the flow map aligns only 35% of the
  time, because consecutive segments are often side by side *across* a wide river rather than along it.
  This was tried first and it is the wrong axis; the mesh's own u gradient is the right one.

---

## 2. `.emat` — the file DayZ actually authors water in

The `.rvmat` beside a DayZ river is 79 bytes and rapified, and its entire content is:

```
PixelShaderID  = "CalmWater"
VertexShaderID = "CalmWater"
```

No stages, no colours, nothing to shade with — which is where the flat green comes from. The `.emat`
sibling is plain text and carries 28 properties for the same surface.

Grammar, censused across **all 207 `.emat` files** in the install:

| | |
|---|---|
| Header form A | `ClassName {` — **164** files |
| Header form B | `material "some/path": ClassName`, brace on the next line — **43** files (postprocess) |
| Body | `Key value...`, **1 to 4 values, never more, in any file** |
| Keys | bare words, or quoted when they contain spaces (**5** such keys exist) |
| Values | numbers, bare words (`translucent`, `alpha`), or `"{GUID}path.edds"` |
| Texture refs | **314**, every one GUID-prefixed |
| Classes | 11 material + effect classes; water is `MatWaterPool` (19) and `MatOceanOld` (7) |

`EmatSource.hpp` reads it and, like `RvMaterialSource`, **interprets nothing** — it records the class,
every key and every token, and stops. Which key means what differs by class, and assigning meaning here
would bake in a guess.

Result over the whole corpus, through `PoseidonTools model emat`:

```
files       207
parsed      207  (100.000%)
properties  2198
textures    314
failed        0
```

### One bug worth recording, because only the denominator found it

Two files — both Sakhal's ice-lake water — nest a block:

```
AlbedoMapTexMat {
 1 1 0 0
}
```

Read flat, that inner `}` closes the material, and **everything after it is silently dropped**. Those
two files lost half their texture references. Nothing errored; a spot check of either file's first
lines would have looked perfect. It surfaced only by counting references across the corpus and
comparing against an independent regex over the raw text — 2 files disagreed, by 2 references each.
With brace depth tracked, the two agree on **0 of 207 files disagreeing**, and the nested tokens become
further values of the property that opened the block.

---

## 3. `.edds` — a DDS header in front of a container that is not DDS

The handover recorded `.edds` as *"a plain DDS container (magic `DDS `...) and the engine already has a
DDS reader"*. The header is real; the payload is not. Handing an `.edds` to `ReadDDSBuffer` does not
fail loudly — it takes the first bytes of the chunk table as mip 0 and returns garbage.

The layout, fitted against the install and closing to the byte:

```
[128-byte DDS header]            dwReserved1[1] == 'ENF1'   (454 / 454 files)
[20-byte DXT10 header]           only when ddspf.dwFourCC == 'DX10'
[mipCount x {FourCC tag, u32}]   chunk table, SMALLEST mip first
[chunk payloads, in table order]
```

Two chunk tags:

* **`COPY`** — stored raw, and its chunk size always equals the size the level's dimensions and block
  format require: **1,846 of 1,846**.
* **`LZ4 `** — `[u32 rawTotal][ {u32 compSize (bit 31 set on the last), lz4 block} ... ]`.

**Mip order is reversed from DDS.** The table runs smallest to largest. Read the other way round, every
texture in the game is a 1×1 smear — a bug that renders rather than one that errors, which is why the
unit test asserts it on level *contents* and not just on extents.

### The part that is not in any format description

`.edds` splits a mip into 64 KiB raw sub-blocks, and **the sub-blocks are linked**: a match in sub-block
N may reference output produced by sub-block N−1. Decoded independently they cannot be, and the failure
is invisible below 64 KiB. That is exactly how it presented — **936 of 1,427** mips decoded, with every
failure on a texture larger than 64 KiB. Decoding the chain into one buffer, with each sub-block allowed
to reach back before its own start: **1,493 of 1,493**.

`Lz4Block` is written from the published block format rather than adapted from the reference
implementation, so no upstream licence attaches — the same reason `Lzo1x` beside it is hand-written
(CORE-007). No new dependency was added.

### Closure

Whole container, every mip, all 454 files:

```
ENF1 marker                  454 / 454   (100.000%)
byte accounting closes       454 / 454   (100.000%)
COPY chunk == natural size  1846 / 1846  (100.000%)
LZ4 mip decodes to natural  1493 / 1493  (100.000%)
files fully decoded          454 / 454   (100.000%)
```

Through the engine's C++ reader, which additionally has to map the pixel format onto `PixelFormat`:

```
files    454
decoded  431  (94.934%)
levels  3141
failed    23     ARGB8888 331   DXT1 55   DXT5 45
```

**All 23 refusals are named pixel-format gaps, not container failures** — 21 are DX10 (BC7 ×16, BC5 ×3,
BC4 ×2) and 2 are 8-bit, and `PixelFormat` has no enumerator for any of them. Each is refused by name
(`DX10 pixel format BC5 (DXGI 83) is not supported by this build`) rather than swallowed.

None of them is a river or pond texture. **Every `.edds` in `water_bliss` (7/7), `water` (1/1) and
`water_sakhal` (5/5) decodes.** The two that matter and do not are `dayzwater1_no` (BC5) and `foam_ca`
(BC4) — both **ocean**, not rivers.

---

## 4. Routing it into the engine, and how far that got

The three pieces above are only useful if a water surface actually reaches the renderer with
them. `EmatMaterialAdapter.hpp` closes that: when a material translates to something that
**binds no textures at all** — the state a `CalmWater` stub produces — the `.emat` sibling is
read and replaces it.

The guard is on "bound nothing", not on the family name. An `.emat` beside a
fully-specified rvmat is not something DayZ writes, but keying on `CalmWater` would mean any
future family with an `.emat` silently loses its own stages, and that failure is invisible.

What maps, and deliberately nothing else:

| `.emat` key | goes to |
|---|---|
| `NormalMap` | `MaterialSlot::NormalMap` |
| `EnvironmentMap` | `MaterialSlot::Environment` |
| `AlbedoMap` | `MaterialSlot::BaseColour` — Sakhal's ice has one; rivers and ponds do not |
| `Color` (RGBA) | `diffuse`, including the alpha as opacity — **and**, when the material has no `AlbedoMap`, a synthesised procedural base colour (below) |

The other ~25 keys (`SunPower`, `WaterExtinction`, `RefractScale`, `Caustic`, the `Stream*`
trio) describe a water shading model this engine does not have. `specular` and `emissive` are
sitting unused and it would be easy to park them there; that would be a claim about their
meaning that nothing has measured, so they stay in the `EmatMaterial` for a water renderer to
read. This is the same rule `RvMaterialSource` follows.

`LoadTranslatedMaterial` bundles parse + translate + `.emat` so the step reaches every backend
from one place, rather than being a fourth line repeated at each call site.

### It works, and here is exactly how far

Booting Enoch through the substituted-world path, the whole new chain is exercised end to end
and the log says so:

```
Wgpu material: normal map bound for dz\water_bliss\ponds\data\enoch_pond.rvmat
               family=CalmWater ( dz\water_bliss\ponds\data\enoch_pond_nohq.edds)
```

That single line is the `.rvmat` stub resolving to its `.emat` sibling, the `.emat`'s texture
reference resolving through the addon bank, and an `.edds` decoding and uploading to the GPU —
none of which happened before. **Zero uncaptured wgpu errors** in that run.

### The albedo that does not exist

That first run also showed the surface still drawing untextured:

```
Wgpu material: no albedo for dz\water_bliss\ponds\data\enoch_pond.rvmat
               (no BaseColour slot and no layer colour) family=CalmWater fallback=none
Wgpu material: section draws UNTEXTURED (fallback=none) model=dz\water_bliss\ponds\enoch_50x50.p3d
```

The cause is not a missing binding — it is that **a DayZ river or pond authors no albedo
texture at all**. Its entire appearance is the `Color` constant, and the renderer's
base-colour resolution wants a texture.

The obvious fix — teach the renderer to fall back to `diffuse` when nothing is bound — is a
change to every backend for a case Real Virtuality had already solved. RV's notation for "this
stage is a flat colour" is a **procedural texture**, `#(argb,8,8,3)color(r,g,b,a,CO)`, and this
engine already parses and generates them (`render::procedural`); three of the seven stages of a
typical Arma 3 Super material are procedural stand-ins, and Arma 3's landing lights are surfaces
whose whole appearance is one.

So when an `.emat` has a `Color` and no `AlbedoMap`, the adapter emits exactly that name. No
renderer code changed; the existing procedural path generates and uploads it. `BindsNothing`
had to learn that a procedural stage counts as bound — it has no `path`, being generated from
its own name, so a path-only test would have made the adapter reject its own output.

### What it looks like, and the finding that came out of looking

A pond was finally captured in frame (Enoch, `--test-world-look-at` aimed at the pond at
X=591 Z=1116 — note azimuth 0 points at **+Z**, which is why two earlier attempts photographed
the wrong direction). Two runs, **identical camera**, either side of the procedural-colour change,
with a rock formation in shot as a control:

| | before | after |
|---|---|---|
| pond region, mean RGB | (58.1, 62.8, 76.5) | **(3.8, 4.4, 5.9)** |
| rock control region | (16.3, 18.3, 22.9) | (16.3, 18.3, 22.9) — *pixel-identical* |
| saturated-green pixels | **0.0000%** | **0.0000%** |
| uncaptured wgpu errors | 0 | 0 |

**The neon green is gone.** The water reads as a large flat quad, clearly a distinct surface from
the terrain, and no longer green in any run.

The control region being pixel-identical is what makes the rest readable: nothing but the pond
surface changed. And what changed is that the pond got **darker**, from luminance 62.8 to 4.4.

That is not a bug, and it is the most useful thing this capture produced. The "before" pale
blue-grey was the **untextured white fallback** — it looked vaguely like water by accident. The
"after" is the colour DayZ actually authored: `Color 0.0392 0.0549 0.0667` is RGB **(10, 14, 17)**,
a near-black blue.

So **DayZ water is authored almost black on purpose**. Practically none of its appearance is
albedo: it comes from what the surface *reflects*. The `.emat` says so plainly — `EnvironmentMap`,
`ReflectionColor 0.2 0.298 0 0`, `FresnelRange 0.468`, plus refraction and depth-scattering terms.

This reorders the remaining work. The adapter fills `MaterialSlot::Environment`, but **nothing in
the renderer binds it** — a log search for an environment or reflection bind returns nothing. Until
a reflection term exists, a DayZ water surface is a correctly-coloured near-black plane, and
**animating its flow would not make it look any more like water**, because there would be nothing
lit to animate. Reflection first, flow second.

The change is therefore a correction in the data and, on its own, a regression in appearance. It is
kept because the pale version was an accident of a fallback and building on that would be building
on a bug; if the old look is wanted meanwhile, the synthesised base colour in
`EmatMaterialAdapter::TranslateEmat` is a single guarded block to drop.

### The reflection DayZ actually uses, and one trap in finding it

`Image::FromFile` now understands `.edds`, so `PoseidonTools image convert foo.edds foo.png` works
and these textures can be *looked at* for the first time. Doing that answered the reflection
question directly.

The env map both water materials name is
**`Enfusion/Graphics/Textures/Water/env_lake_co.edds`** — 256×256, and it is a plain
**equirectangular panorama**: sky and cloud across the top ~40%, a horizon line, smeared olive
ground below. Both materials also agree on `ReflectionColor` (0.2, 0.298/0.3, 0, 0) and
`FresnelRange 0.468`.

That matters more than it looks, because **the engine's water shader already samples an
equirectangular sky env map** with a documented convention (`water.wgsl`: *"Equirect lookup into
the sky reflection env map. Matches fs_sky_env's convention in sky.wgsl"*). DayZ's env map is the
same projection. Whatever path the reflection work takes, the sampling convention is one the
codebase already has and does not have to be invented.

**The trap:** `water_bliss/river/data/enoch_river_env_co.edds` and its pond twin sit right beside
the materials, are named exactly as you would expect the env map to be named, and are **not
referenced by anything**. They are 512×512 *fisheye* images, a different projection entirely.
Wiring one of those up would produce a plausible-looking wrong reflection. The referenced file
lives in `dta/graphics.pbo`, nowhere near the material — the reference has to be followed, not
guessed from the directory listing. This document nearly recorded the wrong file.

## Engine changes

| File | |
|---|---|
| `Foundation/Algorithms/Lz4Block.hpp` | new — LZ4 block decode with linked-sub-block support, fully bounds-checked |
| `Graphics/Textures/EddsReader.{hpp,cpp}` | new — the `.edds` container |
| `Graphics/Textures/DdsImport.{hpp,cpp}` | new — `TextureSourceDDS`, an `ITextureSource` over both containers |
| `Graphics/Rendering/Font/Pactext.cpp` | `.dds` and `.edds` registered in `SelectTextureSourceFactory` |
| `Asset/Formats/Material/EmatSource.hpp` | new — the `.emat` material |
| `Asset/Formats/Material/EmatMaterialAdapter.hpp` | new — `.emat` -> `TranslatedMaterial`, and `LoadTranslatedMaterial` |
| `WgpuRenderer/EngineWgpu.cpp` | both material call sites go through `LoadTranslatedMaterial` |
| `Graphics/Textures/Image.cpp` | `Image::FromFile` reads `.edds`, so the image tools can convert and show them |

Block-compressed levels are kept **compressed** and handed to the renderer as blocks, the way the PAA
path does. Decoding them to RGBA at load time would be simpler, but DayZ's textures are 1k–4k and a
world holds ~28,000 of them, and the open blocker on this branch (Issue 1 in the handover) is the
renderer exhausting device resources at exactly that scale — so inflating every texture 8× on the way
in is the one thing this must not do.

### Tooling

* `PoseidonTools image edds <file|dir>` — decodes and reports container closure. Over a directory it is
  a coverage census, the same shape as `terrain census`.
* `PoseidonTools model emat <file|dir>` — parses `.emat` and reports coverage, `--keys` to tabulate.
* `PoseidonTools terrain objects --model <substring> --basis` — the placement dump that refuted §1.
  `--basis` prints the aside/up/dir rows; without it a placement's orientation is not observable.
* `PoseidonTools model uv <file|dir> [--lod N] [--vertices]` — UV sets per LOD, and their extents
  tabulated over a directory. This is what made the u/v scale split visible and settled the indexing.
  Its shared `LoadOdolStatic` also gives `model selections` revision 54, which it had been refusing.

### Tests

27 new unit tests, all inputs hand-built from the format:
`test_lz4block.cpp` (8), `test_edds_reader.cpp` (8), `test_emat_source.cpp` (10) — plus the nested-block
regression above. The corpus checks that matter live outside CI, where the corpus is.

---

## What is not claimed

* **Water does not reflect, and that is now the blocker.** The adapter fills
  `MaterialSlot::Environment`, but no backend binds it, and DayZ water is authored near-black
  precisely because its appearance is reflection rather than albedo (§4). How to fix that — and why
  routing DayZ's water into the engine's existing water renderer is the wrong answer — is decided in
  [DZ-003](DZ-003-the-water-renderer-is-one-global-plane-so-reflection-belongs-in-the-model-shader-20260814.md).
* **Rivers do not flow.** The flow field is fully decoded and its indexing and axis convention are
  settled (§1), but nothing consumes it. Deliberately *after* reflection: animating a surface with
  no reflective term changes nothing visible.
* **No refraction, depth scattering, caustics or Fresnel.** Every parameter these need is parsed
  and sitting in the `EmatMaterial` unread. All of it is shading work in
  `WgpuRenderer/rust/src/water/`, which another agent is actively editing.
* **No frame showing DayZ water has been inspected.** The evidence in §4 is the material/texture bind
  path in the log, not a picture of a pond. Two capture attempts put the camera somewhere without
  visible water; that is a gap in the verification, not evidence either way.
* **Flow direction, not flow sense.** §1 settles the axis convention and the indexing; it does not
  settle which end is downstream, and the flat geometry cannot. The flow map is the authority there.
* **BC4 / BC5 / BC7 do not load** — 23 files, named above. `PixelFormat` needs enumerators and the
  backends need the formats. Ocean and GUI fonts are affected; rivers and ponds are not.
* **`.edds` mip data is not colour-managed.** The reader returns the stored bytes; nothing here decides
  whether a given map is sRGB or linear, and a flow map and an albedo map must not be treated alike.

## One correction to DZ-001, and one number of its that stands

* **Sakhal is installed.** DZ-001 recorded it as absent. It is present, at `DayZ\sakhal\Addons` — but
  its world ships as **`worlds_sakhal.ebo`**, not `.pbo`, which is why a scan for `.wrp` inside `.pbo`
  archives did not see it. Its other archives (`water_sakhal`, `structures_sakhal`, …) are ordinary PBOs
  and read fine; the five `water_sakhal` `.edds` and both ice-lake `.emat` are in the corpus counts
  above. **The `.ebo` was not opened** — it appears to be a protected archive format, and defeating that
  is not something this work should do. Sakhal's *terrain* therefore remains unavailable, for a
  different reason than DZ-001 recorded.

* **DZ-001's count of 553 is correct**, and an earlier draft of this document wrongly "corrected" it to
  454. The split, which is the part that was not previously recorded and is what the reader has to
  handle:

  | | | |
  |---|---|---|
  | `.edds` | **454** | Enfusion container — all 454 carry `ENF1` |
  | `.dds` | **99** | **genuine plain DDS — 0 of 99 carry `ENF1`** (`graphics` 94, `system` 5) |
  | total | **553** | |

  So both readers are genuinely needed, and `TextureSourceDDS` picks between them on the marker rather
  than on the extension. The `.edds` closure figures above are over the 454; the 99 plain files go to
  `DDSConverter::ReadDDSBuffer` unchanged and are not claimed here.

  This is worth recording as a process note as much as a factual one: the miscount came from extracting
  `.dds` out of `Addons` only — where there are none — and concluding the extension was unused, without
  running the same extraction over `dta` and `sakhal`. The corpus was wrong before the count was.
