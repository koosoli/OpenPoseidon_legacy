# MAT-046 — the Arma 2 and Arma 1 section readers still drop the word that hides a proxy

- **Ticket:** MAT-046 (roadmap §2.5, `docs/roadmap/HANDOVER-20260813.md:195`)
- **Date:** 2026-08-14
- **Status:** investigation only. **Nothing was built, deployed or captured** — a
  performance measurement was running on this workstation. Every number below is
  from offline tools and from the model files themselves.

## The claim

Proxy marker triangles draw because **`Odol49.hpp` and `Odol40.hpp` read the
section's `CommonFaceFlags` word as `(void)reader.read<uint32_t>()`**. The file
states the marker section is hidden; the reader throws the statement away, so
`Section::hints` reaches `ShapeAdapter` as 0 and every downstream skip — CPU and
retained alike — has nothing to skip.

The revision-73 reader already keeps that word (`Odol73.hpp:875`, landed in
`fb6c676`), and revision-73 models hide their markers correctly. Takistan is
revisions 48 and 49. The fix is one line per reader, in the same slot, and it
needs no new predicate anywhere.

Two earlier attempts missed it for the same reason: both went looking in the
named-selection unit, and on this generation the named selections that would name
a LOD-0 marker are **empty in the file**.

## The intended path, end to end

| step | file:line | what it does |
|---|---|---|
| file | `Odol49.hpp:205` | section record: `faceLower, faceUpper, minBone, boneCount, <common point flags>, textureIndex:i16, **CommonFaceFlags:u32**, materialIndex` |
| reader | `Odol73.hpp:195` | `Odol73StaticSection::commonFaceFlags` (shared struct, all revisions) |
| convert | `ODOLLoader.hpp:365` | `section.hints = commonFaceFlags & (IsHidden \| IsHiddenProxy)` |
| IR | `Model.hpp:214` | `Poseidon::Model::Section::hints` |
| shape | `ShapeAdapter.cpp:561` | `shapeSections[s].properties.SetSpecial(int(sec.hints))` |
| **the predicate** | | `properties.Special() & (IsHidden \| IsHiddenProxy)` |
| retained, eligibility | `EngineWgpu.cpp:3407` | `continue` — section not considered |
| retained, bake | `EngineWgpu.cpp:3554` | `continue` — no `WgrModelSection` emitted, so the GPU never draws it |
| CPU direct | `ShapeDraw.cpp:220` | `continue` |
| shadows | `SceneShadowPass.cpp:147`, `Shadow.cpp:437/550/669/728/806` | in `kSkipMask` |

The exact predicate that should exclude a proxy triangle is
`ShapeSection::properties.Special() & (IsHidden | IsHiddenProxy)`, and it is
evaluated in **five** places. All five are correct. All five see zero.

## Where it actually fails — measured, not inferred

### The engine reader reports nothing, on the whole world

`PoseidonTools model inspect --sections` prints `Model::Section::hints`, i.e.
exactly the value `ShapeAdapter` hands to `ShapeSection::properties`. Over every
model Takistan places:

| corpus | sections | sections with a non-zero hint |
|---|---:|---:|
| Takistan, revision 48 | 819 | **0** |
| Takistan, revision 49 | 5,211 | **0** |
| **total** | **6,030** | **0** |

### The file says otherwise, on the same sections

Read straight out of the file bytes with a standalone parser anchored on the
sections array's own count and section 0's face bounds, then cross-checked
record-by-record against the engine tool's `faceLower/faceUpper/textureIndex/
materialIndex` — a mis-anchored parse is rejected, not reported
(`scratchpad/secflags.py`):

| Takistan corpus, 513 distinct models, 438,749 placements | |
|---|---:|
| models carrying proxy records | **40** |
| their placements | 2,251 |
| proxy marker sections, all LOD levels | **348** |
| of those, flagged `IsHidden` or `IsHiddenProxy` in the file | **348 / 348** |
| not flagged | 0 |
| section record could not be located / ambiguous | 0 |
| marker triangles, all LOD levels | 6,313 |
| marker sections on drawable LODs (resolution < 900) | **119** |
| marker triangles on drawable LODs | **1,898** |
| marker sections on LOD 0 | 41 |
| marker triangles on LOD 0 | **789** |
| LOD-0 marker `CommonFaceFlags` histogram | **`0x10002000` × 41** — no variance |

`0x10002000` = `IsHiddenProxy (0x10000000) | NoClamp (0x2000)`.

Placement-weighted, LOD 0: **40,862 marker triangles** across the world if every
instance drew its top LOD. That is the ceiling, not a frame count — one LOD draws
per instance.

Worked example, `ca\structures_e\housek\house_k_1_ep1.p3d` (228 placements):
LOD 0 has 14 proxy records, all naming section 0; section 0 holds exactly 14
faces, `texture -1 material -1`; the file's flag word is `0x10002000`; the engine
reports `flags=0x0 (None)`.

### Positive controls, both directions

| model | revision | file says | engine reports |
|---|---|---|---|
| `a3\soft_f\offroad_01\offroad_01_unarmed_f.p3d` | 73 | marker sections hidden | **28 of 267 sections `0x10000000 (IsHiddenProxy)`** |
| `ca\structures_e\housek\house_k_1_ep1.p3d` | 49 | `0x10002000` | `0x0 (None)` |
| `ca\wheeled\hilux1_civil_3_open.p3d` | 40 | `0x1000c000` | `0x0 (None)` |

Revision 73 proves the conversion, the mask, `ShapeAdapter` and both skips all
work as written. Only the readers for the older generations differ, and they
differ in one token.

### Which path draws them: the retained one, confirmed by where the warning is

The `Wgpu material: section draws UNTEXTURED (fallback=none)` line that produced
the "38 sections" figure is emitted at **`EngineWgpu.cpp:3733`**, inside the
section loop that opens at `EngineWgpu.cpp:3549` and closes at `3759` — the
retained bake loop whose `IsHidden | IsHiddenProxy` skip sits at `3554`. Every
warned section was therefore being baked into a GPU pool mesh, not drawn by the
CPU branch.

`RegisterGpuModel`'s own comment at `EngineWgpu.cpp:3390-3397` confirms the design:
proxy-bearing buildings are deliberately kept on the retained path with
`GpuDrawCoverage::Partial` — the GPU owns the parent's opaque geometry, the CPU
draws only the furniture. So a fix confined to the per-draw branch would have
changed nothing.

**This is not a per-draw/retained split.** Both branches read the identical
`ShapeSection::properties.Special()`. One reader fix covers both by construction.

*Caveat on the "38": the warning dedupes on `model + rvmat`
(`EngineWgpu.cpp:3731`) and every one of these rvmats is `<none>`, so 38 lines
count 38 distinct **models**, not 38 sections.*

## What was ruled out, and how

**1. MAT-044's section-list fallback (`ShapeAdapter.cpp:833-845`) — provably dead.**
Over the 40 proxy-bearing Takistan models: **0 of 6,313** `proxy:` named
selections carry a non-empty `sections` array, and only **160 of 17,816**
selections of any kind do. The array is empty in the file, not lost in
conversion. This confirms the handover's 35,917-of-36,135 figure in kind and
extends it: for *proxy* selections specifically the coverage is zero, not merely
low. This branch can never carry the information and should be deleted.

**2. The named-selection face route (`f161d8d`) — structurally unavailable on the
drawing LOD.** `f161d8d` genuinely fixed the unit (indices, not byte offsets),
but on this generation the arrays it fixed are empty:

| 40 proxy-bearing Takistan models | |
|---|---:|
| `proxy:` selections, all LODs | 6,313 |
| with zero faces **and** zero vertices | 3,658 |
| `proxy:` selections on LOD 0 | 789 |
| of those, zero faces | **726 (92%)** |

The 63 LOD-0 exceptions live in exactly three models —
`ca\misc_e\fort_watchtower_ep1.p3d` (17), `market_stalls_01_ep1` (24),
`market_stalls_02_ep1` (22). So the face route reaches 3 of 40 models. That is
why the named-selection fix was expected to close this and did not.

**3. Not lost after the reader.** `ShapeAdapter.cpp:561` copies `sec.hints` into
`properties` with `SetSpecial` before anything else touches the section;
`LODShape::OptimizeShapes` (`ShapeLOD.cpp:576`) only drops whole LOD levels — it
never merges or rewrites sections; and the ODOL branch never calls
`FindSections()` (`ShapeAdapter.cpp:596`, the `else` arm), so no texture merger
runs on this path. There is nothing between the reader and the predicate that
could eat the bit.

**4. Not a mask problem.** `ODOLLoader.hpp:363-365` masks to
`IsHidden | IsHiddenProxy`. The measured file word `0x10002000` survives that mask
intact.

## Is hiding even correct? Yes — the engine substitutes

A proxy marker triangle is a placeholder that carries a coordinate frame. The
engine resolves it to a real model and draws that instead:

- `ShapeAdapter.cpp:760-798` builds one `ProxyObject` per proxy record, resolving
  the named model and setting the marker's transform.
- `Object::DrawProxies` (`Object.cpp:871-918`) draws each proxy's own shape at
  that transform, then the parent's `Shape::Draw` paints the parent — with the
  marker section skipped at `ShapeDraw.cpp:220`.
- The OFP/MLOD path states the contract outright: `ShapeLOD.cpp:865-866` does
  `face.OrSpecial(IsHiddenProxy | NoTexMerger); face.SetTexture(nullptr);` and
  logs **"Proxy face should be hidden"** if the flag is missing.
- `SceneShadowPass.cpp:380` and `:573`: "the parent holds only an `IsHiddenProxy`
  placeholder."

So the marker is replaced *and* hidden — both, not either. Not hiding it yields
the furniture **plus** a white triangle, which is precisely the reported symptom.
There is no reading on which drawing the marker is correct.

## The fix

Two lines, both in `engine/Poseidon/Asset/Formats/P3D/`. **Another agent is
editing these files for DayZ support — coordinate before touching them.**

**1. `Odol49.hpp`, `ReadOdol49StaticSections`, line 205.** That line contains
*two* `(void)reader.read<uint32_t>()` calls. Change the **second** one — the one
between `textureIndex` and `materialIndex`. (The first is the common *point*
flags and must stay skipped.)

```
section.textureIndex = reader.read<int16_t>(); (void)reader.read<uint32_t>(); section.materialIndex = ...
                                               ^^^^^^ becomes: section.commonFaceFlags = reader.read<uint32_t>();
```

**2. `Odol40.hpp`, `ReadOdol40StaticSections`, line 332.** Same slot, already on
its own line:

```cpp
        section.textureIndex = reader.read<int16_t>();
        (void)reader.read<uint32_t>();            // -> section.commonFaceFlags = reader.read<uint32_t>();
        section.materialIndex = reader.read<int32_t>();
```

Nothing else changes. `ODOLLoader.hpp:365` already masks the field into
`Section::hints` for every revision that fills it; `ShapeAdapter.cpp:561` already
copies it; all five skips already test it. One edit reaches revisions **48, 49,
50, 52 and 54** (Arma 2, Operation Arrowhead, DayZ) and the other reaches **40**
(Arma 1).

**Keep the mask; do not widen it.** The same word carries `NoClamp` (set on every
section measured here), `ClampU/V`, `IsAlphaOrdered` and `ZBias`, and
`render::IsGpuOwnedSectionSpec` classifies retained-vs-CPU routing from this same
spec value — passing the word through whole would reroute sections between draw
paths as a side effect of a fix about hiding markers. Widening is a separate,
separately measured change.

**Two comments become false and should go with it:**
- `ShapeAdapter.cpp:802` — "Oxygen creates sections before marking proxy faces
  hidden, so section hints lack `IsHiddenProxy`." False for revisions 40, 49 and
  73, measured above. Believing it is what sent two attempts into the
  named-selection unit.
- `ShapeAdapter.cpp:833-845` — the MAT-044 `sectionIndices` branch, dead at
  0 of 6,313. Delete it rather than leave a fallback that cannot fire.

## The capture that would prove it

**Offline first (no build of the game needed beyond `PoseidonTools`):**
run `PoseidonTools model inspect --sections` over the 40 proxy-bearing Takistan
models. Today it reports **0 non-zero hints in 6,030 sections**. After the fix it
must report **348 sections with `0x10000000`**, of which **119 on LODs with
resolution < 900** — the same counts `scratchpad/secflags.py` reads out of the
file bytes. If those two numbers do not match exactly, the reader is off by a
field, not fixed.

**Then the runtime capture** (recipe from the Takistan harness — camera
free-fly `2600 700 450 0 0`, look-at `2780 862 371`, hour 12, `--test-mission`
absolute, `--test-type screenshot`, all nine addon-root directories):

Count lines matching `Wgpu material: section draws UNTEXTURED (fallback=none)`
in the log.

- before: **38**
- expected after: **3 or fewer**

The residue is a *different* defect and must not be counted as failure: three
LOD-0 sections that are texture-less, material-less, **not** flagged hidden
(`0x00002000`), and named by no proxy record —
`house_c_1_v2_ep1` section 0 (8 faces), `house_c_12_ep1` section 0 (12 faces),
`house_c_5_ep1` section 0 (1 face). Verified individually here.

A screenshot alone is not acceptance: these are one-triangle specks
(`EngineWgpu.cpp:3723` warns about exactly this — a mosque's 36 markers report
`diag=26` while every triangle is a speck). The log count is the measurement.

## What is explicitly NOT claimed

- **Not** that this is the only source of untextured geometry on Takistan. The
  three sections above are a separate, unexplained defect.
- **Not** anything about Arma 3. Revision 73 already carries the flag correctly
  (28 of 267 sections on the offroad control); this change does not touch it.
- **Not** that the marker vertices leave the GPU buffers. `wgr_mesh_create`
  (`EngineWgpu.cpp:3535`) uploads the whole LOD's vertices and indices; only the
  per-section descriptor is filtered. The triangles stop being drawn; they still
  occupy buffer space.
- **Not** a performance claim. 1,898 triangles over all drawable LODs, 40,862
  placement-weighted at LOD 0, is negligible geometry. This is a correctness fix.
- **Not** verified in a running frame. No build, no launch, no capture — a
  performance measurement was running. Everything above is file-level and
  tool-level evidence.
- **Not** verified that the two routes agree on the 63 LOD-0 markers whose named
  selections *do* carry faces (3 models). `OrSpecial` is idempotent so
  double-marking is harmless, but it was not measured.
- The world-level denominators come from `PoseidonTools terrain objects` —
  **513 distinct models over 438,749 placements**. The world's own model table
  declares 534 models over 438,906 placements; the 21-model / 157-placement
  difference between the table and the placement listing was not investigated
  and does not bear on any number above.

## Harness

Session scratchpad (ephemeral — copy before relying on it):
`…/1a2268b3-876c-478f-a46c-aedb3d4eb9fb/scratchpad/`

- `secflags.py` — reads `CommonFaceFlags` out of ODOL 40/48/49/50/52/54 section
  records by anchoring on the sections count plus section 0's face bounds, then
  validating every record against `PoseidonTools model selections`. Run it on one
  model directly for a per-LOD dump.
- `extract.py` — pulls all 513 Takistan-referenced models out of
  `inspiration/officialarma` into `scratchpad/tak/` (one file at a time; ~90 s,
  no gigabyte-scale writes).
- `census.py` / `census2.py` / `census3.py` — the marker-section, placement-weighted
  and LOD-0 tables above.
- `ab.py` — the engine-side control: what `Model::Section::hints` actually is.
- `tak-objects.txt`, `tak-village.txt`, `tak-model-counts.txt` — placement tables
  from `PoseidonTools terrain objects` (the `--near 2780 862 300` dump is the
  capture's village: 49 models, 995 placements).

---

## Resolution — 2026-08-25, commit `aa4c4c75`

Landed as diagnosed, two lines, eleven days after this record was written. The
prompt was the owner noticing the symptom on an **Arma 1 T-72** — thin triangles
radiating off the hull in a wireframe dump — and observing that the same thing
shows on the Arma 2 map. It does, and for this exact reason.

**The change.** `ReadOdol40StaticSections` (`Odol40.hpp`) and
`ReadOdolA2StaticSections` (`Odol49.hpp`) now assign the CommonFaceFlags word to
`section.commonFaceFlags` instead of `(void)`-discarding it, in the same slot
between `textureIndex` and `materialIndex` that revision 73 has read since
`fb6c676`. Nothing downstream was touched, because — as this record established —
nothing downstream was wrong.

**Coverage.** Revision 40 through `Odol40`; revisions 48, 49, 50, 52 and 54
through `Odol49`, which all funnel into `ReadOdolA2StaticModel`. That is every
generation between OFP and Arma 3: Arma 1, Arma 2, Operation Arrowhead, and DayZ.

**Post-fix witnesses.**

| fixture | rev | sections | flag word |
|---|---|---|---|
| A1 `t72.p3d` (`packages/a1-compat/addons/tracked.pbo`) | 40 | 0, 1, 2 — the three `texture -1 material -1` sections carrying its 6 proxies | `0x1000c000` HIDDEN-PROXY |
| A2 `t72.p3d` (`ALDP_A2_PBOs_…part3/tracked.pbo`) | 48 | 1, 2, 3 | `0x10002000` HIDDEN-PROXY |
| `ca\structures_e\housek\house_k_1_ep1.p3d` | 49 | LOD 0 section 0 | `0x10002000` — the row in the table above recorded `0x0` here |

The house is this record's own named fixture, which makes it the direct
before/after: same file, same section, `0x0` on 2026-08-14 and `0x10002000` now.

**Observability, which was the reason this stayed invisible.** No tool printed
the flag word, so a reader that drops it looks exactly like a file that never set
it — and that is precisely how two earlier attempts went hunting in named
selections instead. `PoseidonTools model selections` now prints
`flags 0x########` per section plus a `HIDDEN-PROXY` / `HIDDEN` tag.

**Not a witness:** `PoseidonTools model render` still draws marker triangles. The
debug wireframe does not consult section hints, so it shows spikes before and
after. Judging this fix by that render is a false negative; use `model selections`
or an in-engine capture.

**Not re-measured.** The 6,030-section, 513-model Takistan sweep above has *not*
been re-run post-fix. The corpus figure stays the pre-fix one. What the fix is
expected to change there — 119 marker sections on drawable LODs, 41 of them on
LOD 0 — is a prediction from the file bytes this record already measured, not a
new measurement. Re-running `census.py` would close it.

**Verification.** Full suite 4319/4320. The single failure,
`Release builds reject --dev before help or option registration`, is pre-existing
and unrelated: it asserts on the *source text* of `AppConfig.cpp`, which this
change does not touch and which last moved in the upstream merge `4f7d22c3`.
Built and deployed via `scripts\Deploy.ps1`; game folder stamped
`feat/consolidated aa4c4c75`.
