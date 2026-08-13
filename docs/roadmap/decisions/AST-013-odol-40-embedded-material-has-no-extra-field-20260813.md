# AST-013 — ODOL revision 40's embedded material is ODOL 73's, and one speculative field was hiding it

**Date:** 2026-08-13
**Status:** Measured, and implemented. Format solved on the whole local A1 corpus; the C++ reader
landed as `engine/Poseidon/Asset/Formats/P3D/Odol40.hpp` (see "The C++ reader" at the bottom).
**Supersedes the open question in:** `HANDOVER-20260813.md` §2.6 ("one unresolved field — the
version-9 material's `texGens` count appears present on some materials and absent on others").

## The claim

Arma 1's ODOL revision 40 embedded material is **byte-identical in layout to ODOL 73's**. There is
no extra field, and `texGens` is neither optional nor ambiguous. The prototype carried a speculative
`matExtraField` u32 after `version`; removing it takes the corpus from 73.3% of LOD bodies closing
to **100%**.

## Measurements

All figures are the whole local A1 corpus: `packages/a1-compat/addons`, 95 PBOs, 2,580 ODOL models.

**Material version is uniform.** Reading only the *first* material of every LOD — its `version` sits
at a known offset immediately after the material count, so it cannot be corrupted by a downstream
desync — gives one value and no other:

| version | LODs | models |
|---|---:|---:|
| 9 | 6,932 | 1,870 |

There is no revision-6, -7, -8, -10 or -11 material anywhere in Arma 1. That matters because the BIS
reference layout branches hard at v<8 (transforms and textures interleave) and at v>8 (`nTexGens` is
read as an independent count). With the population uniformly v9, **the layout is fully determined and
has no optional fields at all**.

**The extra u32 is not there.** A LOD carrying two or more materials is self-validating: a wrong
layout desyncs after the first, and every later material then fails to report version 9 with a
printable name. Over 3,665 multi-material LODs (14,958 materials):

| variant | LODs closed |
|---|---:|
| ODOL 73 layout, no extra field | **3,665 / 3,665 (100.0%)** |
| with `matExtraField` u32 | 1,194 / 3,665 (32.6%) |

**`texGens` always equals `nStages`.** Across every material parsed, the pair never diverges:
`(3,3)` ×6,270, `(1,1)` ×3,396, `(5,5)` ×2,816, `(2,2)` ×1,299, `(4,4)` ×1,119, `(6,6)` ×58. It is
always present, because v9 > 8.

**Whole-corpus LOD closure.** `lodbody.py`, requiring each LOD to close *exactly* on its declared
boundary:

| configuration | models fully parsed | LODs closed |
|---|---:|---:|
| `matExtraField=True` (as inherited) | 1,103 / 2,580 | 13,699 / 18,697 (73.3%) |
| `matExtraField=False` | 2,579 / 2,580 | 18,696 / 18,697 (99.995%) |
| ` + material cap 64 → 256` | **2,580 / 2,580** | **18,697 / 18,697 (100%)** |

18,379 materials read, all version 9.

## How the earlier reading went wrong

The handover recorded the symptom as "the `texGens` count appears present on some materials and
absent on others". That description is what a **desynced stream** looks like from inside, and it
pointed the investigation at the one field that is in fact unconditional.

The speculative `matExtraField` consumed 4 bytes that are not in the file. The *first* material of a
LOD still parsed — its name and version are read before the phantom field, and the 100 bytes of
colour that follow absorb the shift without tripping any plausibility check. Only when the parser
reached `nStages`/`nTexGens`, four bytes out, did the counts become nonsense; whether they looked
"present" or "absent" depended entirely on what the misaligned window happened to land on. So the
field that was blamed was the first field downstream of the actual error, not its cause.

This is the same shape as two traps already recorded in this repo: a wrong reading that produces a
*plausible* value rather than an obvious failure. The defence that worked was refusing to judge the
layout from one material and instead requiring a **chain** of them to close — 3,665 LODs where a
wrong answer had to stay wrong 3,665 times in a row.

The last outlier was not a format problem either: `characters.pbo : material_wounds.p3d` genuinely
carries **154** embedded materials (a wounds material library; `mat0` is a valid v9
`ca\characters\data\camelpilot_w2.rvmat`). The prototype's cap of 64 was simply lower than the data.

## Consequences for the C++ reader

`ReadOdol73EmbeddedMaterials` (`engine/Poseidon/Asset/Formats/P3D/Odol73.hpp:680`) **already parses
A1 rev-40 materials correctly as written** — the Python parser that closed 100% is a line-for-line
mirror of it, minus the `version >= 10` / `>= 11` branches, which cannot fire at v9. No new material
reader is needed. Two changes are required:

1. **`Odol73.hpp` capped embedded materials at 64.** The A1 corpus contains a 154-material model,
   so this threw on real data. Raised to 256. *(Done.)*
2. **`OdolRevision.hpp` reported revision 40 as `Recognised`** ("No reader is claimed: nothing of
   revision 40 parses today"). It is now `NarrowSubset`. *(Done.)*

The LOD body reader itself, which the prototype specifies completely, is `Odol40.hpp`:
`quantisedUv=False, condensedNormals=True, sectionCommonPointFlags=True, sectionMaterialIndex=True,
sectionAreas=True, multiUv=True, packedNormals=False, stCoords=True, vertexBoneRefs=True,
neighbourBoneRefs=True`.

## The C++ reader

`Odol40.hpp` is a port of the prototype, dispatched from `ODOLLoader::loadFromBuffer` and reaching
the same `Poseidon::Model::Model` IR every other format lands in. `OdolRevision.hpp` now reports
revision 40 as `NarrowSubset`, which is also what opens the runtime path: `P3DFormatDetector`
defers to `DescribeOdolRevision`, and `ModelCache` consults the detector before any reader runs.

Two things the 100%-closure census never had to check, because the prototype produced structs
rather than an engine model, were measured before the port relied on them. Both are unanimous
across the corpus:

| question | measured |
|---|---|
| units of a section's `faceLower`/`faceUpper` | 2 bytes for the count, 2 per index: rebuilding at 2/2 reproduces the LOD's declared `faceDataSize` for **18,697 / 18,697** LODs, and all **52,394** section bounds land on a face boundary |
| `restDataSize` versus bytes consumed | declared − consumed = **4**, for **18,697 / 18,697** LODs and never another value; the field counts itself |

The second is why `ReadOdol40StaticRestData` can end with an exact size check rather than the
"close enough" one the A2 reader needs.

## A latent bug found in passing, now fixed

The reference gates two things our reader does unconditionally, both only for material versions
below 8:

- `stageID` is read at `Odol73.hpp:713` for every version; the reference reads it only at **v≥8**.
- The reference **interleaves** `[StageTransform, StageTexture]` per stage at **v<8**, where ours
  always reads all textures and then all transforms.

Neither can fire on Arma 1 (uniformly v9) or on Arma 2/3 (v9+), so this is latent, not live.

Both are now fixed to match the reference, with the stage-texture gates factored into
`ReadOdol73StageTexture` so the version rules live in one place. The witness is synthetic rather
than a corpus file — `test_odol40_arma1.cpp` writes a v6 and a v7 material the way the reference
writes them and requires the reader to end exactly on the sentinel that follows. That is a weaker
witness than a real file and is worth saying plainly: it proves our reader agrees with the
reference's layout, not that any shipped file uses it. What it does buy is that the first v6/v7
file to arrive fails loudly if the layout is wrong, instead of reading four bytes out of place and
producing something plausible — which is the exact trap this ADR was written about.

## Reproducing

```bash
python a1odol/matver.py     # first-material version histogram (no desync possible)
python a1odol/matchain.py   # variant A vs B over multi-material LODs
python a1odol/fullb.py      # whole-corpus LOD closure with matExtraField off
```
