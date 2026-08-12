# MAT-037 — ShapeAdapter split sections with the wrong stream's stride

**Date:** 2026-08-12
**Supersedes the open question in:** `MAT-036-oa-vegetation-renders-as-shards-20260812.md`
**Related:** MAT-030, MAT-020, WLD-016

## What MAT-036 left open

MAT-036 eliminated four causes for Arma 2/OA foliage rendering as opaque bark-like
shards and named UV sourcing as the leading remaining hypothesis. That hypothesis
is **disproved**, and the real cause is one line of arithmetic.

## UV sourcing is not the defect

Per-section decoded UV bounds, traced from the loader:

```
OA  t_amygdalusc2s_ep1  LOD0  uvSets=2
  section 0  tex=0  uv0 u[0.000,1.000] v[0.000,0.999]      crown, whole atlas
  section 1  tex=0  uv0 u[0.000,0.500] v[0.000,0.999]      crown, left half
  section 2  tex=1  uv0 u[-3.787,4.000] v[-29.680,2.272]   trunk, tiled bark
A3  t_ficusb2s_f       LOD0  uvSets=2
  section 0  tex=1  uv0 u[0.000,0.414] v[0.477,0.937]      crown sub-rect
  section 1  tex=0  uv0 u[0.000,1.000] v[-5.734,8.360]     trunk, tiled bark
```

Both are internally consistent and correct: crowns sample the atlas in [0,1],
trunks tile. Texture indices and face→section mapping are correct in both, as
MAT-036 already established. No per-stage `uvSource` change was made, because
nothing here justifies one.

## The actual cause

`ShapeAdapter` rebuilds per-section face counts by walking the face stream and
comparing a running byte position against the section bounds:

```cpp
constexpr int kFilePolyPropsSize = 8;               // sizeof(PolyProperties) on x86
fileBytePos += kFilePolyPropsSize + sizeof(VertexIndex) * (1 + nVerts);
```

`VertexIndex` is `short`, so that is **16 bytes for a triangle**. But the bounds
it compares against are byte offsets into the *source* ODOL face stream, whose
stride the loader records per revision:

| source | header | index | triangle | quad |
|---|---:|---:|---:|---:|
| ShapeAdapter's assumption (v7) | 8 | 2 | **16** | 18 |
| ODOL revision 73 | 4 | 4 | **16** | 20 |
| ODOL revisions 48/49/50/52 | 2 | 2 | **8** | 10 |

Two different streams measured in one unit. Revision 73 agrees on triangles by
coincidence, which is why **Arma 3 has been correct here by luck** — and why it
still drifts on quads. The Arma 2 family is out by exactly 2×, so every section
boundary lands at twice its true face index.

On the Takistan almond, LOD0's true split is 5,104 / 1,914 / 2,398 faces. The
walk produced 2,552 / 957 / 5,907, so **3,509 crown faces were drawn in the
trunk's section, with the trunk's bark texture**. That is the "mass of bark-like
shards": it was the tree's crown, wearing its own bark.

## The fix

The loader already computes each section's exact face range, face by face,
through the offset table it has already validated against the LOD's declared
face-data size — and records `firstFace`, `faceCount` and `faceRangeKnown` in the
model IR. `faceRangeKnown` exists for precisely this and had never been read.

`ShapeAdapter` now uses those counts when every section reports them and they
account for the whole face stream, and falls back to the byte walk otherwise.
That removes the stride assumption rather than adding a per-revision branch to
it. The OFP v7 path never sets `faceRangeKnown`, so it keeps the existing walk
byte-for-byte.

## Evidence

- OA almond through `--test-model`: a bark-shard mass becomes a tree with a
  correct trunk, branch structure and green foliage.
- Takistan ground capture: the foreground bush becomes a bush.
- A3 ficus through the same harness: visually identical before and after; the
  3.50/255 mean difference is the scene's animated grass and cloud.
- A3 Stratis airbase, same camera: 0.87/255 mean, all of it moving water and
  cloud. Census unchanged at 350 of 350 over 163,953 placements.
- OFP Preview-0 WGPU smoke passed.
- `[odol]` 22 passed / 2 skipped; `[Formats]` 23 passed / 1 skipped;
  `[graphics]` 10,534 of 10,536 assertions, the two failures pre-existing and
  unrelated (a hidden `[.fixture]` DDS helper, and a stale `sizeof(SVertex)==36`
  assertion against a 60-byte struct).

## Still open

The OA canopy is now correctly textured but sparse: its leaf atlas has 0.0% fully
opaque texels against Arma 3's 12.5%, and 83.3% of its non-clear texels reach the
0.5 alpha reference against Arma 3's 93.9%. A fixed 0.5 reference erodes an
alpha-to-coverage-authored atlas, and mip levels make it worse with distance.
That is the next vegetation question and it is shared, not A2-specific.

`ca\structures_e\misc\misc_market\cages_ep1.p3d` still renders solid black. It is
unaffected by this fix — the lattice silhouette is correct, so alpha is cutting
out and the surviving bars are simply black. Separate defect, not yet diagnosed.
