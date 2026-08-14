# AST-016/017 scoping — the animation data is already parsed; the converter drops it, and the skinning it drops changed shape between Arma 1 and Arma 2

**Date:** 2026-08-14
**Status:** Scoping only. Nothing implemented, nothing changed in the tree. Every number below is
measured on this workstation and carries its denominator; where a thing could not be measured with
the tools that exist, it says so instead of estimating.
**Covers the two untouched ledger tickets:** `AST-016` (model.cfg, CfgSkeletons, CfgModels,
AnimationSources) and `AST-017` (RTM + BMTR/RTMB), both `PLANNED`/`HOLD` since the roadmap was
written (`docs/roadmap/status-ledger.yaml:1629` and `:1660`, both with empty `evidence`,
`test_ids`, `decision_record`).

---

## The claim

Four separate findings, in the order they change the plan:

1. **The ODOL readers already parse essentially the whole rig.** Skeleton name, bone list with
   parents, the full animation-class table with its type-dependent payloads, both bone↔animation
   lookup tables, per-bone axes, LOD proxies with their bone and section bindings, the two halves
   of the sub-skeleton mapping, and the per-vertex bone/weight array. On revisions 40, 48, 49, 50,
   52, 54 and 73. None of it is skipped — each block is read and the LOD is required to close on
   its own declared boundary.

2. **`ODOLLoader` then throws all of it away.** `engine/Poseidon/Asset/Formats/P3D/ODOLLoader.hpp`
   contains **zero** references to `skeletonName`, `bones`, `animations`, `hasAnimations`,
   `header.proxies`, `subSkeleton*` or `vertexBoneRefs`. The only thing `convertOdolStatic`
   (`ODOLLoader.hpp:180`) reads out of `source.directory` is `source.directory.model` — the
   preamble (`:182`). The gap between "parsed" and "usable" is one function, and the model IR it
   writes into (`engine/Poseidon/World/Model/Model.hpp`) has no field to put a skeleton in.

3. **The 2001 animation machinery is reusable in its entirety, and is already GPU-skinned.** The
   RTM player, the skeleton, the weight tables, the palette upload and the WGSL skinning path all
   consume the engine's `Shape`/`NamedSelection` object model and nothing else — `RtAnimation.cpp`
   and `Animation.cpp` include no P3D or ODOL header. The single seam is one line:
   `Skeleton::Prepare` resolves **bone name string → named selection → vertex list + weights**
   (`engine/Poseidon/World/Simulation/Animation/RtAnimation.cpp:753`).

4. **And that seam is exactly what stops working at Arma 2.** Measured, two models, same skeleton
   name (`OFP2_ManSkeleton`), same bone names:

   | model | revision | LOD 0 selections | bone-named selections with vertices |
   |---|---|---:|---|
   | `civil_1_tshirt.p3d` (A1 `characters.pbo`) | 40 | 89 | `spine` 102 verts, `spine2` 88, `spine3` 286, `head` 862, `leftshoulder` 97, `leftarm` 137, … |
   | `BlackHead.p3d` (A2 `characters.pbo`) | 48 | 42 | `spine3` **0**, `head` **0**, `head_injury` **0** |
   | `Common\basicbody.p3d` (A3 `characters_f.pbo`) | 73 | 90 | `spine` **0**, `spine1` **0**, `spine2` **0**, `spine3` **0**, `head` **0**, `leftshoulder` **0**, `leftarm` **0**, `leftarmroll` **0**, `leftforearm` **0**, … `pelvis` **0** — 90 of 90 empty |

   Arma 1 still carries skinning in named selections, the way OFP does. From ODOL 48 onward the
   binariser moves it into `vertexBoneRefs` plus the sub-skeleton mapping and leaves the
   bone-named selections in the file as empty shells. **The names survive; the membership does
   not.** Feeding an Arma 2 or Arma 3 character through the existing path today would produce a
   fully-populated `Skeleton`, a `WeightInfo` of the right size, an all-zero weight table, and a
   character that stands in bind pose forever with no error anywhere.

So the size of the gap is small in one direction and large in the other. Arma 1 is close to free.
Arma 2, Arma 3 and DayZ need a second skinning source path that does not exist, and the RTM reader
covers **39 of 6,157 (0.6%)** of Arma 3's animation files.

---

## What this does NOT claim

- **Nothing here has been run.** No build, no launch, no capture. This is a read of the tree and a
  census of local corpora.
- **It does not claim any of the four generations would animate correctly** if the wiring were
  done. It claims which data is present and where the plumbing stops.
- **It does not claim Arma 1 characters will animate from named selections alone.** The selections
  have vertices; whether their `vertexWeights` arrays are populated (as opposed to defaulting to
  255 in `ShapeAdapter.cpp:433`) was **not measured** — no tool in the tree prints them. That is
  Stage 1's first fixture, not an assumption to build on.
- **It does not claim the BMTR layout is correct** — only that a complete MIT-licensed reference
  implementation of it is sitting in the working tree (below), and that the format is a quaternion
  + half-float encoding rather than the 3×4 float matrix the current reader expects.
- **It says nothing about OFP/CWA's own skinning population.** The census could not measure it:
  ODOL revision 7's ModelInfo layout is not implemented in the Python harness, and 878 of CWA's
  1,400 models are PBO-compressed with no LZSS decoder in the harness. 0 of 1,400 measured.
- **DayZ is out of scope of AST-017 as written.** It ships **0** `.rtm` and **8,484** `.anm` files,
  magic `FORM` on 8,484 of 8,484. That is a different container family entirely and needs its own
  ticket, not a widening of this one.
- **It does not scope Stage 5** (see below) with any confidence. That stage is honestly unscoped.

---

## 1. What the readers already capture, read-and-discard, and refuse

### Parsed and retained in the reader structs

All of this is in the revision-73 structures, which the Arma 1, Arma 2 and DayZ readers reuse:

| data | struct | file:line |
|---|---|---|
| skeleton name, `isDiscrete` | `Odol73LodDirectory` | `Odol73.hpp:100`, `:101` |
| bone list with **parent** | `Odol73Bone{name, parent}` | `Odol73.hpp:41`, filled `:585-590` |
| animation classes, type-dependent payloads | `Odol73AnimationClass` | `Odol73.hpp:51`, read `:437-492` |
| `[resolution][bone] → animations` | `Odol73Animations::bonesToAnimations` | `Odol73.hpp:85`, read `:497-513` |
| `[resolution][animation] → bone` + per-bone axis pair | `animationsToBones`, `axes`, `hasAxis` | `Odol73.hpp:88-92`, read `:518-537` |
| LOD proxies incl. `boneIndex`, `sectionIndex`, `namedSelectionIndex` | `Odol73Proxy` | `Odol73.hpp:115` |
| sub-skeleton mapping, both directions | `Odol73StaticLodHeader::subSkeletonsToSkeleton` / `skeletonToSubSkeleton` | `Odol73.hpp:130-131` |
| section bone range | `Odol73StaticSection::minBone`, `boneCount` | `Odol73.hpp:182` |
| **per-vertex skinning** — count + 4×(bone, weight) | `Odol73StaticRestData::vertexBoneRefs` | `Odol73.hpp:262-267` |
| neighbour bone refs (32 raw bytes, unconverted) | `neighbourBoneRefs` | `Odol73.hpp:268` |
| named-selection per-vertex weights | `Odol73NamedSelection::vertexWeights` | `Odol73.hpp:227` |

The shared animation reader is `ReadOdol73Animations(BinaryReader&, uint32_t revision)` at
`Odol73.hpp:429`. Revision 40 calls it with `40` (`Odol40.hpp:182`); the A2 family calls it with
its own revision (`Odol49.hpp:134`); revision 73 uses the default (`Odol73.hpp:612`). Two version
gates inside it: `revision >= 56` adds `animPeriod`/`initPhase` (`:451`), `revision >= 55` adds the
hide animation's second float (`:482`). An unknown animation type is a **refusal**, not a skip
(`:488`) — the payload width determines every later offset.

I checked this against the MIT-licensed BI reference in the tree
(`.tmp-uksfta-bis/BIS.P3D/ODOL/AnimationClass.cs`, `Skeleton.cs`): the C++ matches it field for
field, including the `version > 40` gate on the obsolete-pivots string, which `Odol40.hpp` correctly
omits (see its header comment at `Odol40.hpp:21`) and `Odol73.hpp:591` correctly reads.

### Read and discarded — the actual gap

`ODOLLoader::convertOdolStatic` (`ODOLLoader.hpp:180-233`) and `convertOdol73Mesh`
(`:235-453`) between them copy geometry, materials, sections, named selections (vertices, weights,
faces, section list) and named properties into the IR. They copy **nothing** from
`source.directory` except the preamble, and **nothing** from `source.lods[i].header` except the
bounding box, hints and textures.

Specifically dropped for every ODOL revision 40/48/49/50/52/54/73:

- `directory.skeletonName`, `directory.skeletonIsDiscrete`, `directory.bones`
- `directory.hasAnimations`, `directory.animations` (classes, both lookup tables, axes)
- `lod.header.proxies` — note the v7 path *does* convert proxies (`ODOLLoader.hpp:688`
  `convertProxies`), so proxies are a regression on the later revisions specifically, not a
  missing feature generally
- `lod.header.subSkeletonsToSkeleton`, `lod.header.skeletonToSubSkeleton`
- `lod.sections[].minBone`, `lod.sections[].boneCount`
- `lod.rest.vertexBoneRefs`, `lod.rest.neighbourBoneRefs`

The IR has no home for most of it. `Poseidon::Model::Model` (`Model.hpp:328`) has no skeleton,
no bone array, no animation table. `Mesh` (`Model.hpp:250`) has `selections`, `proxies` and
`frames` — where `frames` is `AnimationFrame{time, positions}` (`Model.hpp:241`), the **v7
vertex-morph** keyframe, not a bone matrix. That is filled only by the v7 path
(`ODOLLoader.hpp:712` `convertFrames`) and consumed by `ShapeAdapter.cpp:231-250`.

### Outright refusals

- Keyframes on the later revisions are read for length and **discarded on the spot**:
  `ReadOdol40Keyframes` (`Odol40.hpp:378`) and `ReadOdol49Keyframes` (`Odol49.hpp:217`) seek past
  the vertex data. Revision 73's equivalent does the same. So the one animation mechanism that
  *does* reach the engine on the v7 path is thrown away on all the others.
- Unknown animation type → `UnsupportedOdolRevision` (`Odol73.hpp:488`).
- ODOL 75 (`Recognised`) and 68 refuse at dispatch (`OdolRevision.hpp:88`, `ODOLLoader.hpp:106`).
- The capability matrix still records `ODOL73_SKINNING: UNSUPPORTED` — "0 skinned models parsed; a
  populated vertex- or neighbour-bone array raises rather than being skipped"
  (`docs/roadmap/compat-c1-capability-matrix.md:56`). That row is **stale**: WLD-012 removed the
  raise (`:147`, and `WLD-012-odol73-world-prop-coverage-20260810.md:31-33`). It is now read and
  kept. Correcting that row is a five-minute job and should be Stage 0 of anything here, because
  it is the row a future agent will read to decide the work is bigger than it is.

---

## 2. The engine's own animation machinery, and whether it can be fed

### What exists

| piece | file:line | note |
|---|---|---|
| RTM reader | `engine/Poseidon/Asset/Formats/RTM/RTMReader.hpp:49/101/125` | `RTM_0100` and `RTM_0101` only (`:57`, `:62`) |
| RTM IR | `RTMReader.hpp:36` | `boneNames` (strings), `phases[].transforms[]` = 12 floats, `stepX/Y/Z` |
| RTM player | `RtAnimation.hpp:179` `AnimationRT` | phases, phase times, keyframe bracket + lerp (`RtAnimation.cpp:790`, `:1057`) |
| skeleton | `RtAnimation.hpp:125` `Skeleton` | **a flat `AutoArray<RStringB>` of bone names. No parents, no bind pose, no rest transform.** |
| skeleton population | `RtAnimation.cpp:532-536` | bones are appended **from the RTM's own bone list**, in discovery order |
| weight table | `RtAnimation.hpp:39` `AnimationRTWeights` | one entry per vertex, max 4 influences (`:29`) |
| **the seam** | `RtAnimation.cpp:740-762` `Skeleton::Prepare` | `FindNamedSel(GetBone(i))` → `AddSelection(shape, selIndex, i)` |
| weight derivation | `RtAnimation.cpp:121-156` | from `sel.Weight(si) * (1/255)`, evicting the weakest when >4 |
| CPU skinning | `RtAnimation.cpp:864` / `:959` | writes `shape->SetPos(i)`/`SetNorm(i)` from `OrigPos`/`OrigNorm` every frame |
| **GPU skinning** | `RtAnimation.cpp:1008-1025` | `vb->SetSkinData(...)` + `vb->SetPalette(...)`, skips the CPU loop |
| WGPU implementation | `EngineWgpu.cpp:587` (`SetSkinData`), `:629` (`SetPalette`), `:1873-1892`, `:4139-4155` | 4×u8 bone index + 4×u8 weight per vertex; 128-matrix palette blocks |
| WGSL | `engine/WgpuRenderer/rust/src/shaders/skin.wgsl`, `gfx3d/shader3d.wgsl:219` | linear blend skinning; shadow casters pose on GPU too |
| config-driven prop animation | `Animation.hpp:158` `AnimationType`, `:179` `AnimationRotationType`, factory `Animation.cpp:1317` | reads `type`/`selection`/`axis`/`begin`/`end`/`angle0`/`angle1`/`animPeriod` |
| phase state machine | `Animation.hpp:196` `AnimationInstance`, `Animation.cpp:1399-1420` | rate-limited `_phase` → `_phaseWanted` |
| gameplay entry point | `Simul.cpp:258`, `:1334`, `:1364`; `Vehicle.hpp:297/350` | `animationPhase` / `setAnimationPhase` SQF commands already exist |

### The answer

**Structurally tied to the v7 model layout: no.** Evidence:

- `RtAnimation.cpp` and `Animation.cpp` include no `P3D/` header of any kind. The only format
  include in the animation subsystem is `RTMReader.hpp`.
- Everything the animation code touches is engine-level: `LODShape::NLevels/Level`,
  `Shape::NPos/OrigPos/OrigNorm/SetPos/SetNorm/FindNamedSel/NamedSel`. Those are populated by
  `ShapeAdapter::convertToLODShape` from the format-neutral `Model` IR, identically for MLOD, ODOL
  7, 40, 48, 49, 50, 52, 54 and 73.
- The bone↔geometry bind is **by name**, not by index or offset (`RtAnimation.cpp:753`), so it does
  not care what produced the selection.
- Weights already flow through the adapter: `ShapeAdapter.cpp:425-440` builds `SelInfo(vertIdx,
  weight)` from `NamedSelection::vertexWeights`, which the ODOL 40/48/49/50/52/54/73 converter
  fills at `ODOLLoader.hpp:426`.

**Tied to the OFP *content convention*: yes, completely.** The convention is "a bone is a named
selection with the same name". That convention holds for OFP and Arma 1 and is measurably false
from ODOL 48 onward (the table in the claim above). `AnimationRTWeights` can only be built by
`AddSelection`, and `AddSelection` can only read a `::NamedSelection`. There is no second
constructor that takes a per-vertex bone/weight array.

**Two hard capacity limits, both reached by real Arma 3 content:**

- `AnimationRTPair::_sel` is a **`char`** (`RtAnimation.hpp:11`). Signed. Bone index ≥128 corrupts
  silently.
- `WGR_PALETTE_SIZE` is 128 (`wgpu_renderer.hpp:239`), `kReservedBone = 127`
  (`EngineWgpu.cpp:468`), and `SetPalette` **silently truncates** anything above that
  (`EngineWgpu.cpp:631-634`).

Measured maximum declared bone count per model, from the ODOL ModelInfo bone table:

| corpus | max bones in one skeleton |
|---|---:|
| Arma 1 (ODOL 40) | 94 |
| Arma 2 base / OA / DLC (48/49/50/52) | 132 |
| DayZ (ODOL 54) | 159 |
| **Arma 3 (ODOL 73)** | **244** |

Arma 2 already exceeds 127. Arma 3 nearly doubles it. Widening `_sel` to `uint16_t` and the palette
to 256 is a prerequisite, not a polish item, and the palette widening crosses the C ABI
(`wgpu_renderer.hpp:1330`) and the WGSL.

---

## 3. The format gap, per generation

### Where the skeleton and animation *definitions* live

`model.cfg` is a **binarize-time input, not a shipped runtime asset**. Measured:

| location | `model.cfg` files |
|---|---:|
| inside **all** PBOs of all five corpora combined | **13** |
| loose in the licensed sample-model packages (`inspiration/officialarma/A1SM_*`, `A2SM_*`) + Arma 3 Samples | **400** |

Breakdown of the 13: Arma 1 2, Arma 2 base 2, Arma 2 OA 5, OA DLC 1, Arma 3 1, CWA 0, DayZ 0.

That is the most consequential scoping fact for **AST-016**. For binarised content — which is all
shipped content — `CfgSkeletons` and `CfgModels`' `class Animations` have already been resolved
into ODOL ModelInfo by the binariser: `skeletonName`, the bone list *with parents*, and the
`Odol73AnimationClass` table with `type`, `source`, `sourceAddress`, `minPhase`/`maxPhase`,
`minValue`/`maxValue`, `animPeriod`/`initPhase` and the per-type payload. **All of it is already
parsed** (§1). AST-016 as scoped in the roadmap
(`PoseidonWGPU_Modernisation_Roadmap_v4.5.3.md:3576` — "CfgSkeletons → SkeletonIR, CfgModels →
ModelConfigIR, AnimationSources → AnimationControllerIR") is therefore mostly needed for
**unbinarised MLOD workflows and modding**, not for playing shipped content. It should be
de-prioritised relative to AST-017 and to the plumbing work in §5.

What the engine reads from config today, for comparison:

- `CfgModels` is read at `ShapeLOD.cpp:334` (`properties`) and `ShapeLOD.cpp:956-975` +
  `ShapeSetup.cpp:1012` (`sections`/`sectionsInherit`). **Only those two.** No `skeletonName`, no
  `class Animations`.
- `CfgSkeletons`, `AnimationSources`, `skeletonName` and `SkeletonSource` produce **zero** hits in
  `engine/` and `apps/` C++ outside the ODOL readers.
- OFP-style per-vehicle animation comes from a `CfgVehicles` `Animations` array read at
  `Simul.cpp:258`, and the factory implements **exactly one type**: `"rotation"`. Anything else is
  `Fail("Unknown animation type")` (`Animation.cpp:1327`). ODOL declares ten types (0–9).

Config readers available: `ParamFile` reads BI's `\0raP` at version ≥2
(`ParamFileParse.cpp:930`), which is the OFP lineage. `Poseidon::Asset::Config::ArmaRapReader`
(`engine/Poseidon/Asset/Formats/Config/ArmaRap.hpp`) is a separate, narrow reader for the Arma-era
`\0raP` container producing a neutral tree, currently consumed only by RVMAT. Whether `ParamFile`
can read an Arma-era binarised `model.cfg` was **not tested**; the known data point is that
`PoseidonTools config debin` fails on Arma 3 RVMATs (which are the same container), so assume it
cannot until measured.

### The RTM family

| magic | encoding | reader status |
|---|---|---|
| `RTM_0100` | 8-byte magic, 1 step float, per-phase per-bone 32-byte name + 12 floats | **read** (`RTMReader.hpp:57`) |
| `RTM_0101` | as above with a 3-float step | **read** (`RTMReader.hpp:62`) |
| `BMTR` v3 | LZO-compressed arrays; per-bone transform = compressed quaternion + 3 half-floats = **14 bytes** | **no reader anywhere in the tree** |
| `BMTR` v4 | + metadata string array + `AnimKeyStone` array | none |
| `BMTR` v5 | + explicit per-array compression flag | none |
| `RTMB` | **does not exist.** No file with that magic in any corpus; the ledger title's "RTMB" is a name for `BMTR`, whose *class* in the reference implementation is called `RTMB` | n/a |
| `.bmtr` extension | **0 files in every corpus.** BMTR is a magic inside a `.rtm` file | n/a |

A complete MIT-licensed reference for BMTR is already checked out in the working tree at
`.tmp-uksfta-bis/BIS.RTM/Format/RTMB.cs` (51 lines) with its supporting
`.tmp-uksfta-bis/BIS.Core/Math/TransformP.cs` and `.tmp-uksfta-bis/BIS.RTM/Format/AnimKeyStone.cs`.
That is the same repository (`github.com/UKSFTA/UKSFTA-BIS`, MIT, © 2017 Braini01) whose ODOL
reader `Odol40.hpp` was ported from per `AST-012D`. This substantially de-risks AST-017: the
format is not being reverse-engineered, it is being ported, and the project has done that once
already.

The shape difference that matters: RTM stores an absolute 3×4 **matrix** per bone per phase and the
existing `AnimationRTPhase::LoadFromRTM` (`RtAnimation.cpp:236`) `memcpy`s 12 floats straight into
a `Matrix4` behind a `static_assert(sizeof(Matrix4)==sizeof(float)*12)` (`:246`). BMTR stores a
**quantised quaternion + half-float translation**. Conversion is a quaternion→matrix build, not a
copy, and it loses the ability to express any non-rigid component (there is none in either format,
so this is a note rather than a problem).

### The per-LOD skinning arrays inside ODOL

| generation | skinning carried in | measured |
|---|---|---|
| OFP / CWA (ODOL 7) | named selections (`sel[i]`, `Weight(i)`) | **not measured** — no rev-7 ModelInfo reader in the harness |
| Arma 1 (ODOL 40) | **named selections**, populated | `civil_1_tshirt.p3d` LOD 0: 89 selections, bone-named ones carry 59–862 vertices each |
| Arma 2 / OA (48/49/50/52) | `vertexBoneRefs` + sub-skeleton mapping; named selections present but **empty** | `BlackHead.p3d` (rev 48): 85 selections across all LODs, **85 empty** face lists; `spine3`/`head` 0 verts on both graphical LODs |
| DayZ (ODOL 54) | same family as Arma 2 (per `DZ-001`) — **not directly measured** | — |
| Arma 3 (ODOL 73) | `vertexBoneRefs` + sub-skeleton mapping; named selections present but **empty** | `basicbody.p3d` LOD 0: 90 selections, **90 with 0 vertices**, including every `OFP2_ManSkeleton` bone name |

`vertexBoneRefs` is a count plus eight bytes of paired (bone, weight) — **4 influences maximum**,
which matches the engine's `ARTWMaxSize` of 4 exactly (`RtAnimation.hpp:29`). The bone indices in
it are into the **LOD's sub-skeleton**, not the model skeleton; `subSkeletonsToSkeleton`
(`Odol73.hpp:130`) is the remap and is already parsed.

---

## 4. Corpus denominators

Two independent measurements were taken (a subagent's harness and my own PBO walker, written
separately) and agree on every figure they share — Arma 1 1,379 RTM / 2,581 P3D and Arma 3 6,157
RTM / 39 `RTM_0101` came out identical from both.

### Animation files, by corpus

| corpus | PBOs | `.p3d` | `.rtm` | `RTM_01xx` | `BMTR` | `.anm` | `model.cfg` |
|---|---:|---:|---:|---:|---:|---:|---:|
| CWA retail (base archives) | 136 | 1,400 | 739 | **739** | 0 | 0 | 0 |
| CWA Demo (`packages/Demo`) | 14 | 455 | 517 | **517** | 0 | 0 | 0 |
| Arma 1 (`packages/a1-compat/addons`) | 95 | 2,581 | 1,379 | **1,379** | 0 | 0 | 2 |
| Arma 2 base (`inspiration/officialarma/ALDP_A2_*`) | 53 | 3,330 | 2,989 | 8 | **2,981** | 0 | 2 |
| Arma 2 OA (`ALDP_A2OA_*`) | 35 | 1,538 | 425 | 0 | **425** | 0 | 5 |
| Arma 2 OA DLC (`ALDP_A2OADLC_*`) | 55 | 444 | 79 | 0 | **79** | 0 | 1 |
| Arma 3 retail (`D:\SteamLibrary\...\Arma 3`) | 546 | 11,080 | 6,157 | 39 | **6,118** | 0 | 1 |
| DayZ (`D:\SteamLibrary\...\DayZ`) | 133 | 8,203 | **0** | 0 | 0 | **8,484** (`FORM`) | 0 |

Zero PBOs failed to parse in any corpus. `packages/a1-compat/addons` and
`inspiration/officialarma/ALDP_A1_PBOs_*` share 95 of 95 basenames — the same content; do not sum
them. The CWA `Remastered` subdirectory reports identical asset counts from a different PBO count
— the same archives; do not sum those either.

**BMTR version split, Arma 3:** 6,117 v5, 1 v4, of 6,118. Arma 2's 3,485 loose+packed BMTRs sampled
as v3. So a reader needs v3, v4 and v5; the reference implementation covers all three plus v≥5's
compression flag.

**The headline denominator:** the existing RTM reader covers **39 of 6,157 (0.6%)** of Arma 3's
animation files and **8 of 3,493 (0.2%)** of Arma 2's. It covers **1,379 of 1,379 (100%)** of Arma
1's and **739 of 739 (100%)** of CWA's.

### Skinned-model population, by corpus

A non-empty `skeletonName` with a bone table in ODOL ModelInfo is the model-level declaration that
a model is rigged. Measured by reading the ModelInfo prefix per the repo's own reader layouts:

| corpus | `.p3d` | parsed | **declares a skeleton** | distinct skeletons | max bones | total bones |
|---|---:|---:|---:|---:|---:|---:|
| Arma 1 (ODOL 40) | 2,685 | 2,684 | **302 (11.3%)** | 52 | 94 | 12,681 |
| Arma 2 base (ODOL 48) | 3,330 | 3,330 | **568 (17.1%)** | 180 | 132 | 24,789 |
| Arma 2 OA (48/49) | 1,538 | 1,538 | **428 (27.8%)** | 172 | 132 | 21,735 |
| Arma 2 OA DLC (49/50/52) | 444 | 444 | **139 (31.3%)** | 47 | 132 | 9,687 |
| Arma 3 (ODOL 73) | 11,080 | 10,420 | **1,369 (13.1% of parsed)** | 603 | **244** | 64,939 |
| DayZ (ODOL 54) | 8,203 | 8,203 | **1,405 (17.1%)** | 240 | 159 | 99,605 |
| CWA (ODOL 7) | 1,400 | **0** | **not measured** | — | — | — |

Unmeasured, explicitly: 660 of 11,080 Arma 3 models (640 PBO-compressed with no LZSS in the
harness; 20 that are the 14 ODOL-75 and 6 ODOL-68 models — parsed = 10,420 = exactly the ODOL-73
population), 1 of 2,685 Arma 1 (an MLOD), and all 1,400 CWA.

Most common skeletons: Arma 1 `ofp2_manskeleton` 110, `weapon` 55, `car` 33. Arma 2 base
`ofp2_manskeleton` 98, `default` 75, `rifle` 50. Arma 3 `ofp2_manskeleton` 342,
`church_04_f_skeleton` 28, `cargo_skeleton` 24. DayZ `dayztemporaryskeleton` 586,
`skeleton1doors` 83, `skeleton3doors` 55.

### The world-prop population, for contrast

From `WLD-012`, Stratis + Altis, 350 referenced models: **35 declare model-level animations**, 38
carry per-vertex bone references, 35 carry neighbour bone references. From `WLD-016`, Takistan:
54 of 534 models were blocked purely on the model animation table. So the animated fraction of
*world props* is roughly 10%, and they are doors, gates, shipping containers, wind socks, street
lamps and runway lights — single-bone rotations and hides. The heavily-rigged population
(characters, weapons, vehicles) is in the 11–31% figures above and is not in any world census.

---

## 5. A staged plan

Ordered by unblocked-value ÷ risk. Each stage states what it reads, what it still refuses, what
fixture proves it, and what it does not claim.

### Stage 0 — correct the stale capability row (hours)

`compat-c1-capability-matrix.md:56` says `ODOL73_SKINNING: UNSUPPORTED — a populated vertex- or
neighbour-bone array raises rather than being skipped`. WLD-012 removed that raise. Rewrite the row
to say what is true: the arrays are read and retained and nothing consumes them. Add the same row
for revisions 40 and 48–54.
**Refuses:** nothing; documentation only.
**Proof:** the row cites `WLD-012`'s commit and the struct at `Odol73.hpp:262`.
**Does not claim:** any skinning works.

### Stage 1 — carry the rig into the model IR (small, mechanical, high leverage)

Add to `Poseidon::Model::Model` a `Skeleton{name, isDiscrete, vector<Bone{name,parent}>}` and an
`Animations` block mirroring `Odol73Animations`; add to `Mesh` a `vertexBoneRefs` array, the
sub-skeleton mapping, and per-section `minBone`/`boneCount`. Populate them in
`convertOdolStatic`/`convertOdol73Mesh`. Also convert `lod.header.proxies` — the v7 path already
does (`ODOLLoader.hpp:688`) and the later revisions do not, which is a plain omission.
**Reads:** everything already parsed.
**Refuses:** nothing new; no behaviour change — the engine still ignores the new fields.
**Fixture:** extend `tests/unit/.../test_odol_named_selections.cpp` and add a struct-level test in
the style of `ODOLLoader::convertStaticModel` (which `ODOLLoader.hpp:65` says exists precisely so a
conversion defect can be isolated from a container defect). Assert on `civil_1_tshirt.p3d` (rev 40)
and `basicbody.p3d` (rev 73) that the skeleton name, bone count and parent chain arrive intact, and
on a Takistan door prop that the animation classes and their types arrive.
**Does not claim:** anything animates. This stage moves bytes; it changes no pixel.
**Risk:** low. Nothing consumes the new fields yet.

### Stage 2 — BMTR reader (medium, and the highest coverage-per-unit-risk in the plan)

Port `.tmp-uksfta-bis/BIS.RTM/Format/RTMB.cs` into `RTMReader.hpp` as a second entry point
producing the **same `RTMAnimation`** the existing path produces: detect `BMTR` in
`readRTMHeader`, handle v3 (LZO arrays), v4 (metadata + `AnimKeyStone`), v5 (explicit compression
flag), and convert quaternion + half-float translation into the same 12-float `BoneTransform`.
`Foundation::Lzo1x::Decompress` already exists (`Odol73.hpp:323` uses it).
**Reads:** BMTR v3/v4/v5.
**Refuses:** unknown BMTR versions, and any array that does not decompress to its declared size —
the same discipline the ODOL readers use.
**Fixture:** a corpus closure census in the AST-012D style — decode every `.rtm` in the local
corpora and require bone count × phase count × 14 bytes to account for the declared array exactly.
Denominator is fixed and known: 3,485 Arma 2, 6,118 Arma 3. Plus the existing
`tests/fixtures/rtm/*.rtm` must keep passing byte-identically, and `apps/fuzzers/Fuzzer/fuzz_rtm.cpp`
should get a BMTR seed.
**Does not claim:** that any of those animations play. It claims the file decodes and closes.
**Risk:** low-to-medium. The layout is documented in-tree; the risk is LZO framing and the
half-float/quaternion decode conventions, both of which a closure census catches immediately.

### Stage 3 — widen the bone-index and palette ceilings (small, but it crosses the ABI)

`AnimationRTPair::_sel` `char` → `uint16_t` (and reconsider the packed `VerySmallArray` size,
`RtAnimation.hpp:29`). `WGR_PALETTE_SIZE` 128 → 256, with `kReservedBone` following, across
`wgpu_renderer.hpp`, `EngineWgpu.cpp` and the WGSL. Replace the silent truncation at
`EngineWgpu.cpp:631` with a diagnostic.
**Reads:** n/a.
**Refuses:** a skeleton above the new cap should log and refuse, not truncate.
**Fixture:** an OFP soldier renders byte-identically before and after (the `devtest*.abel` dev
missions are frame-deterministic to 0.054/255 per `MAT-035`, so a pixel A/B is available); a
synthetic 200-bone skeleton drives the palette path without corruption.
**Does not claim:** that a 244-bone Arma 3 rig works — only that the container no longer caps it.
**Risk:** medium. It touches the C ABI and the shaders, and `CORE-NEG-003` exists because ABI
struct changes have bitten here before.

### Stage 4 — a second skinning source: build `AnimationRTWeights` from `vertexBoneRefs`

This is the stage that makes Arma 2/3/DayZ characters possible at all. Add a constructor path
alongside `AnimationRTWeights::AddSelection` that takes the IR's per-vertex bone refs plus
`subSkeletonsToSkeleton`, remaps sub-skeleton indices to model-skeleton indices, and populates
`_data` directly. Choose the source per LOD by measurement, not by revision: if the bone-named
selections are populated use them (Arma 1, OFP), otherwise use `vertexBoneRefs`.
**Reads:** the Stage-1 IR fields.
**Refuses:** a LOD that declares a skeleton but has neither populated selections *nor*
`vertexBoneRefs` — that is a case nobody has characterised and should surface, not silently produce
a bind-pose model.
**Fixture:** `basicbody.p3d` and `civil_1_tshirt.p3d` through `ShapeAdapter`, asserting that the
resulting `WeightInfo` has a non-zero influence for ≥99% of vertices in both cases, and that the
Arma 1 model produces the *same* table from either source where both are present (if both are
present — unmeasured, see §Traps).
**Does not claim:** correct posing. A correct weight table with a wrong bone order still produces a
mangled character.
**Risk:** medium-high. Two index spaces (sub-skeleton and skeleton) that look alike — the exact
shape of `WLD-017`'s terrain-material bug.

### Stage 5 — bone ordering, hierarchy and the actual pose (the hard one)

**This stage is not confidently scoped and should not be estimated.** The unknowns:

- The engine's `Skeleton` is a **flat, unparented, discovery-ordered** bone list built from
  whichever RTM loaded first (`RtAnimation.cpp:532-536`). ODOL declares a **parented** bone list in
  a fixed order. `vertexBoneRefs` indexes the LOD sub-skeleton, which maps to the *ODOL* order.
  BMTR carries bone names, which map to neither by construction. Three orderings have to be
  reconciled and only one of them is currently authoritative anywhere.
- RTM/BMTR phases are absolute per-bone transforms, so hierarchy composition may be unnecessary at
  playback — but `skeletonIsDiscrete` and the `Odol73AnimationClass` sources (`sourceAddress`,
  `minPhase`/`maxPhase`, ten types against the engine's one) are a *procedural* animation system
  layered on top, and that composition order is not established anywhere in this tree.
- `AnimationType::CreateObject` implements one of ten declared types (`Animation.cpp:1320`).
  Implementing types 1–9 is straightforward per-type maths; deciding how a config-declared
  `AnimationSource` binds to a gameplay controller is not, and is the substance of AST-016's
  `AnimationControllerIR`.

Recommendation: after Stage 4, spend one session producing a *measurement* record — pose one Arma 1
character and one Arma 3 character with an identity palette and with a single known keyframe, and
capture. Then scope Stage 5 from what that shows. Do not plan it now.

### Explicitly deferred

- **DayZ `.anm` (`FORM`)** — 8,484 files, a different container. New ticket.
- **AST-016's config side** — 13 shipped `model.cfg` files across every corpus. Needed for
  unbinarised/mod workflows and for the `AnimationSources`→controller binding in Stage 5. Not on
  the critical path to making shipped content move.
- **ODOL keyframes on later revisions** — currently seeked past (`Odol40.hpp:378`,
  `Odol49.hpp:217`). Cheap to add to Stage 1; no known consumer needs them, since the v7
  vertex-morph path (`Model.hpp:241` → `ShapeAdapter.cpp:231`) is the only thing that consumes
  `Mesh::frames` and no later-revision model has been shown to use them.

---

## 6. What will go wrong

This repo's failure mode is consistent and it is worth naming precisely: **a structure that is
shared across revisions in name turns out to differ in one field, and the difference is invisible
because something downstream resynchronises.** `MAT-037` (one face-stream stride assumed for three
different strides; revision 73 was correct *by coincidence* on triangles and wrong on quads),
`WLD-016` (the animation table assumed revision-73-shaped, when 49/50/52 predate two version
gates), `AST-012D` (a speculative `u32` that took corpus closure from 100% to 73.3%). Predictions,
in descending order of confidence:

1. **The one that already happened, above.** "Bone name = named selection name" is true for OFP and
   Arma 1 and false from ODOL 48 onward, and the failure is **silent**: `Skeleton::Prepare` finds
   the selection by name, `AddSelection` iterates zero vertices, `Normalize` sets `_isSimple = true`
   over an empty table, and `ApplyMatricesSimple` transforms every vertex by `matrices[0]`. No
   assert, no log, a plausible-looking model that never moves. Any stage that touches skinning must
   assert on *influence coverage*, not on table existence.

2. **Two index spaces that look like one.** `vertexBoneRefs` indexes the LOD's sub-skeleton;
   `animationsToBones` and `Odol73StaticSection::minBone` index the model skeleton; the engine's
   `Skeleton` indexes RTM discovery order. Three spaces, all `int32`/`uint8`, all plausible when
   confused. This is `WLD-017` ("terrain materials are not ground layers") and `AST-019` ("face
   indices are not byte offsets") in a third costume. Every conversion between them should be a
   named function with a test, not an implicit assignment.

3. **The bone-count ceiling will be hit before anyone is looking for it.** Arma 2 already declares
   132-bone skeletons and Arma 3 declares 244, against a `char` index and a 128-slot palette that
   **truncates without a word** (`EngineWgpu.cpp:631`). The first Arma 3 character to reach the
   skinning path will produce a model whose upper bones are all bound to whatever matrix happened
   to be at the truncated index. Stage 3 must land before Stage 4 is tested on Arma 3 content.

4. **BMTR's version gates will bite the same way ODOL's did.** v3 adds LZO, v4 adds metadata + the
   keystone array, v5 adds an explicit compression flag *per array*. Reading a v3 file with v5's
   flag consumes one extra byte and every later offset shifts. The mitigation is the one this
   project already uses: require each compressed array to close exactly on its declared size, and
   census the whole corpus rather than testing one file. The measured population makes that easy —
   6,118 Arma 3 and 3,485 Arma 2 files, versions 3/4/5 all represented.

5. **The retained/GPU-driven path has no animation gate.** `EngineWgpu::RegisterGpuModel`
   (`EngineWgpu.cpp:3354`) contains no reference to `IsAnimated` or `NAnimationPhases` (verified by
   grep), and `SceneObjectCreated` (`:3871`) admits any `obj->Static()` object. A building with an
   animated door is `Static()`. So a door that starts working on the CPU path may render frozen on
   the GPU-driven path, or worse, work in a `--dev-map` and not in the world. This is a
   *prediction*, not a measurement — nothing animated has been put through that path — but it is
   cheap to check and expensive to discover late. Note also
   `dev-map-has-an-empty-retained-set`: world-space renderer features measure as inert under
   `--dev-map`, so the dev map is not a valid witness here.

6. **Arma 1 may not be as free as it looks.** `civil_1_tshirt.p3d`'s selections have vertices, but
   whether they carry meaningful `vertexWeights` was not measured — `ShapeAdapter.cpp:433` defaults
   an absent weight to 255, which turns a soft selection into a hard one. `Model.hpp:159-173`
   already records that exact hazard for MLOD ("Decoding it on a guess would silently reweight every
   skinned MLOD model"). Measure before assuming.

7. **The `frames` field is a decoy.** `Mesh::frames` and `LODShape::_phase` are the **v7
   vertex-morph** mechanism, not bone animation, and `test_shape_adapter_odol_animation.cpp` tests
   exactly that. An agent reading "ODOL morph frames become animation phases" and concluding the
   animation pipeline is partly wired will be wrong. It is a different mechanism that happens to
   share the word.

8. **`RTMB` will send someone to a hex editor.** The ledger title for AST-017 says "RTM +
   BMTR/RTMB". There is no `RTMB` magic in 20,000+ animation files across five corpora, and no
   `.bmtr` file extension either — 0 of 0. `RTMB` is the *class name* in the reference
   implementation for the format whose magic is `BMTR`. The ticket title should be corrected.

---

## Appendix — how the numbers were measured

- **PBO enumeration:** a Python walker over the PBO entry table (asciiz name + 5×`u32`, `Vers`
  header-extension handling, data offsets accumulated after the terminator), then a magic read at
  each entry's offset. Written twice, independently, by two agents; the shared figures agree.
  Script: `<scratchpad>/pbocensus.py`.
- **RTM magic:** first 24 bytes of each `.rtm` entry, searching for `BMTR` or `RTM_`; the BMTR
  version is the `u32` after the magic. PBO-compressed entries (275 in Arma 3) show the magic at a
  small offset rather than at 0, which is why the scan is a search rather than a compare.
- **Skeleton census:** the ODOL ModelInfo prefix up to and including the bone table, per the layouts
  in `Odol40.hpp:144-160`, `Odol49.hpp:113-121` and `Odol73.hpp:557-592`.
- **Named-selection contents:** `dist/x64-win-rwdi/PoseidonTools.exe model selections <file>` — the
  tool AST-019 added, run on three models extracted from `packages/a1-compat/addons/characters.pbo`,
  `inspiration/officialarma/ALDP_A2_PBOs_DPL_APL_part2/.../characters.pbo` and
  `D:\SteamLibrary\steamapps\common\Arma 3\Addons\characters_f.pbo`.
- **Corpus roots:** `packages/{Demo,a1-compat}`, `inspiration/officialarma/*`,
  `D:\SteamLibrary\steamapps\common\{ARMA Cold War Assault, Arma 3, Arma 3 Samples, DayZ}`. No
  retail Arma 1 or Arma 2/OA install exists on this machine — those generations are covered only by
  the licensed sample PBOs under `inspiration/officialarma`. The CWA install contains project
  working directories (`@a3bisect`, `@a3stratis`, `@a3terrain`, `a3\`, `tmp\`, `_EXTRA\`,
  `Samples_F\`); they are excluded from the CWA row. A first pass that did *not* exclude them
  reported one stray `BMTR v5` inside "CWA" — it was Arma 3 content sitting in the CWA folder, and
  it is the reason the exclusion is worth stating.
- **Not measured, listed once more so it is not mistaken for zero:** CWA/OFP skinned-model count
  (0 of 1,400 measured); 660 of 11,080 Arma 3 models; Arma 1 named-selection *weight* population;
  whether Arma 1 models also carry `vertexBoneRefs`; whether `ParamFile` can read an Arma-era
  binarised `model.cfg`; DayZ's `.anm`/`FORM` internal layout.
