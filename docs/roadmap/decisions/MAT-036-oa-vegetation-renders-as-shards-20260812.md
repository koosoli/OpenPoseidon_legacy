# MAT-036 — Arma 2/OA foliage renders as opaque shards, and four causes it is not

**Date:** 2026-08-12
**Related:** MAT-030 (TreeAdv family), MAT-020 (TreeAdv lighting semantics),
`RND-alpha-routing-misclassifies-a3-foliage-20260809.md`
**Status:** Symptom isolated and reproducible; root cause NOT established.
No production change made.

## The symptom

Official OA `ca\plants_e\tree\t_amygdalusc2s_ep1.p3d` renders as a mass of
opaque, dark, mottled polygon shards with jagged cut edges. Arma 3's
`a3\plants_f\tree\t_ficusb2s_f.p3d` renders correctly — green leaves, clean
cutouts — through the **identical** harness:

```
PoseidonGame.exe -C <install> --render wgpu --no-dev --addon-root <OA roots> \
  --test-mission <install>/dev-missions/devtest.abel \
  --test-model "ca\plants_e\tree\t_amygdalusc2s_ep1.p3d" \
  --test-type screenshot --screenshot <png> --screenshot-delay 12
```

So this is not a harness artefact, and it is not "OA content is different art".
Both models are ODOL, both use the same shader families, and one works.

## What the two assets have in common

`--test-model` reports the same structure for both: `TreeAdv` crown sections and
a `TreeAdvTrunk` section, `schema=true`, Stage1 NormalMap + Stage2 MacroAmbient,
and **no Stage0** — the crown's colour is the section's face texture. The OA leaf
atlas decodes correctly to a normal 4x3 grid of green branch cards on a grey
background; the source art is not at fault.

## Four causes eliminated, with the measurement that eliminated each

**1. Alpha misclassification.** It is real and it is the one measured difference
between the two assets:

| | A3 ficus `_ca.paa` | OA almond `_can.paa` |
|---|---|---|
| format | DXT5 | DXT5 |
| clear (a=0) | 86.5% | 88.0% |
| opaque (a=255) | 12.5% | **0.0%** |
| partial | **1.0%** | **12.0%** |
| `ClassifyAlpha` | CUTOUT | **BLEND** |

`ClassifyAlpha`'s 2%-partial threshold sends OA foliage to the blend route and
lets A3's through. But the existing shader-family override handles exactly this:
`GpuSectionMaterialUsesLeafCards` resolves the OA material to `TreeAdv` and
returns true (traced live), `HasAlphaHoles()` is true, and the texture is not
bark-named — so `materialCutout` is true and the section gets `alphaRef = 0.5`.
The override fires. It is not the gap.

**2. TreeAdv state vectors folded as generic lighting.** OA authors genuinely
non-neutral values where A3 does not — OA crown `ambient [0.05 0.57 0.30]`,
`diffuse [0.50 0.40 0.45]`, `emissive [2.60 2.30 2.00]`, `power 300`; A3 crown
`diffuse [0.50 0.50 0.50]`, `emissive [1.00 1.00 1.00]`, `power 28`. MAT-020
established these are family parameters, not lighting. Suppressing the
`surfMat->Combine` fold for TreeAdv-family sections in `ClassifyGpuSection`
changed the render **not at all**, which is consistent with MAT-020's finding
that the HDR path ignores per-material terms. Change reverted; it fixes nothing.

**3. Wrong section→texture index in the A2 ODOL reader.** Traced. It is correct:
LOD0 sections 0 and 1 (the crown) carry `textureIndex=0` — the leaf atlas — and
section 2 (the trunk) carries `textureIndex=1`.

**4. Wrong face-stream offsets.** Also correct, and self-checking. LOD0 has 9,416
faces; `faceDataSize` is 75,328 = 9,416 × 8, matching the A2 record size of
2 header bytes + 3 × 2 index bytes, and the section face ranges partition it
exactly: [0, 40832) = 5,104 faces, [40832, 56144) = 1,914, [56144, 75328) = 2,398.
`convertOdolStatic` already throws if the rebuilt offsets disagree with the
declared size, and it does not throw here.

That accounts for 7,018 of 9,416 LOD0 faces being crown geometry correctly bound
to the leaf atlas — while the render shows mostly bark-like shards.

## Where to look next

The leading remaining hypothesis is **UV sourcing**. The A2 rest-data carries
`uv0` plus `uvSetCount - 1` extra sets, and the TreeAdv MacroAmbient stage is
authored `uvSource tex1`. If the crown sections sample the wrong UV set — or if
the A2 16-bit UV dequantisation lands in the wrong part of the atlas — each card
would show an arbitrary region of a 2048² atlas whose background is flat grey and
whose twigs are dark grey-brown. That is what the render looks like, and it is
consistent with every measurement above.

The decisive experiment is to dump the decoded UV range per section for both
models and compare against the atlas layout. A second, cheaper check is to render
the OA crown with the base-colour material debug view.

Note the A2 UV dequantisation uses `(u + 32767) / 65536`; the conventional BI form
is `(u + 32767) / 65534`. The difference is ~3e-5 and cannot explain this, but it
should be corrected when the area is next touched.

## Not a regression

Nothing here changed. Both attempted fixes were reverted after measuring that they
changed nothing, and no A3, OFP or OA behaviour differs from the previous commit.
