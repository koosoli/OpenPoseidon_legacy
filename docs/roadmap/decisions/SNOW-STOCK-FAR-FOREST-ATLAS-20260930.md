# Bounded stock far forest crown snow candidate

Status: **SOURCE CANDIDATE. GPU composition/numeric checks, full engine build and installed visual acceptance pending.** This is not general merged-forest or all-map tree coverage.

## Actual source evidence

The installed CWA `DTA/Data3D.pbo` was extracted offline and inspected with the existing `PoseidonTools model inspect --all --simulation-source-info` and `model uv --vertices` commands. Thirteen `les` forest models have 45 visual LODs, all with zero visual named selections. Collision `componentNN` selections do not establish visual per-tree ownership. At far distance the original assets reduce many trees into forest-strip cards; connected-component centroids cannot recover canonical tree identities or faithful per-tree height bounds across those LODs.

Only these source tuples are admitted:

| Model | Original visual LOD / resolution | Actual face texture | Audited region |
| --- | --- | --- | --- |
| `data3d\les ctverec.p3d` | 3 / 40 | `data\stromky2.pac` | Upper foliage of its forest strip; lower exposed trunks remain dry |
| `data3d\les ctverec pruchozi_t1.p3d` | 2 / 12 | `merged\00002.paa` | Upper foliage in the left atlas half only |

Loaded-engine UV diagnostics show the first far LOD as eight seam-duplicated vertices on a single 50m-wide card, approximately 21.85m high. The second far LOD has twelve vertices on 25–50m forest-strip walls. These are authored billboard regions, not reconstructed individual trees. The decoded `stromky2` image places crowns above the exposed trunk strip. The merged image contains a full-height forest strip on the left, a smaller forest strip top-right, a ground tile middle-right and an unrelated broadleaf card lower-right. **The entire right half is excluded**, including its smaller forest; no generic atlas-wide top-height treatment applies.

Inspected immutable source SHA256 values (audit evidence, not a runtime content hash gate):

| Source | SHA256 |
| --- | --- |
| `les ctverec.p3d` | `3b54146d532865159e152bd9e8177fc06fe1a6f13cac44ae7e7c716ce1a68b90` |
| `les ctverec pruchozi_t1.p3d` | `8ed210c8c9a631cd3f38c2fe91b5c3f153575ca9961819a695a597a275ffc276` |
| `stromky2.pac` | `c65cf64463c55a8fe493b13ff2fce888c0449937f89e71c5e3da3dac267b3640` |
| `00002.paa` | `51a32fa09eb99e8225b29c5e93c3ea4c80aa9ba130bf363a03caf196b7f219c5` |

## Admission and transport

`ForestSnowSurface.hpp` admits exact normalized model/LOD/resolution/texture tuples. The final retained section must already be a cutout, retain its actual uploaded face albedo and have no RVMAT, layered/normal/specular-map or conflicting section flags. A visual replacement source path does not inherit admission from the simulation-facing texture name. A substituted/failed/constant albedo remains excluded. This does not change section ownership, selected LOD, cutout classification or the existing loader's malformed-section repair behavior.

Only intact, static, primary instances of the exact built-in `ForestPlain`/`Forest` classes obtain the owner proof. Their existing terrain-plane/skew transformation is recognized explicitly; arbitrary subclasses, entities, proxies and mode-2 conformance do not inherit it. The audited visual LOD must be loaded with its original resolution and have no proxies. The models' historical `MAYANIMATE` flag is not treated as arbitrary pose authority. The engine's actual `ForestPlain` transformation changes positions/normals for static terrain conformity, while these authored UV masks remain unchanged.

Two unused high bits in the existing section flags carry atlas category 1/2. Existing `conform2.w` carries owner proof 2, independently of individual-tree proof 1. The existing `tree_snow.y` varying carries a separate negative sentinel only for admitted forest instances. No public ABI/layout growth, extra buffer, added vertex input/varying, crown-centre reinterpretation or CPU cluster exposure lease is introduced. Private flags pass through the existing retained registration copy.

The fragment mask uses original UVs: `stromky2` feathers out between V=.27 and .42; merged-left between V=.24 and .38. The merged atlas edge feathers out between U=.48 and .50 to avoid painting the adjacent regions through mip bleed. Lower exposed trunk regions, the whole merged right half, invalid coordinates and unknown categories remain zero. The conservative upper envelopes intentionally leave some lower crown foliage bare. Visible twigs/branches photographed inside those upper crown regions cannot be independently classified; this is not wood-pixel segmentation or volumetric branch snow.

## Physical shelter and appearance

Only the existing dedicated physical weather depth map establishes exposure, per fragment. Missing/pending publication, out-of-range depth/UV, guarded borders and retained output overflow return zero. The current map's retained/resident plus direct submission remains the physical scope; this does not certify every unloaded world shelter. An actual roof over only part of a 50m forest card can mask that part independently. Neither artistic AO openness nor the existing individual-tree CPU ray lease supplies a whole-cluster fallback.

Actual snow deposition and the existing smooth altitude snow line drive coverage. Existing RGB-only tree snow shading is reused, with no added glitter, powder normal detail, random/time seeds, geometry, wind, alpha, A2C, discard, depth or shadow changes. This preserves the stock billboard geometry's limitations. The atlas arithmetic adds a small fixed UV envelope; eligible snow fragments add one existing physical-map comparison lookup. There is no additional weather pass or per-frame CPU ray census. No measured performance claim is made.

`WGR_FOREST_SNOW_ATLAS=0` provides a same-binary category ablation without affecting the existing individual-tree snow path. `WGR_TREE_SNOW_TRACE=1` emits bounded `FOREST_SNOW_OWNER` and admitted `FOREST_SNOW_ATLAS` records for actual source/LOD/material proof.

## Verification and remaining gates

The focused standalone CPU policy checks exact positive tuples and rejects wrong assets, LODs, resolutions, texture paths, material/face replacement gates, owner identity/static/intact/class gates, proxies represented by absent ownership, unloaded levels and unsupported conform modes. Shader tests are added for actual production atlas helpers, feather boundaries, lower trunks, merged ground/unrelated/right-half regions, invalid categories/coordinates, map-unavailable/roof-closed states and global tree-snow ablation. **The new GPU tests have not been run by this source-only author.** Existing consumer composition and alpha/wind/normal contract checks remain applicable.

Root acceptance must run shader composition and the actual GPU numeric test, full build, then matched installed `WGR_FOREST_SNOW_ATLAS=0/1` arms. Require trace proof of both owner and selected material tuple, snow-active physical-map ready/no-overflow proof, normal exit and stable installed hashes. Inspect medium/far/top views of an actual admitted cluster: crowns whiten, lower trunks and alpha gaps remain intact, ground/unrelated atlas regions remain unchanged. Include a partial overhead roof control, missing/disabled-map negative, snow-off negative, camera return and the transition into neighbouring unsupported LODs. Atlas-relative snow may visibly switch off at an unsupported LOD; no cross-LOD consistency claim precedes those controls.

Other forest models, other LODs of these two models, non-retained/direct-only draws, native merged forests and unsupported material routes remain bare. Same-path modified models/textures are not byte-certified by runtime admission; this source slice targets the inspected stock asset semantics and must not be generalized to arbitrary mods. Expanding coverage requires additional authored region/geometry evidence, not loose component connectivity or a blanket cluster-height mask.
