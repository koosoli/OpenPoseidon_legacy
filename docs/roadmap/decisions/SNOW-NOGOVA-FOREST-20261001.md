# Nogova forest source candidate and compacted visual LODs

Status: **SOURCE CANDIDATE; expanded WGSL/GPU execution, full build and installed visual acceptance pending.** Extends [the Data3D source family](SNOW-STOCK-FOREST-FAMILY-20260930.md), without claiming that source admission proves the owner's white top-view appearance.

## Actual map and stock source census

Original installed `AddOns/Noe.pbo/noe.wrp` object rows were extracted and inspected offline with the existing terrain-object command; model names were filtered explicitly from the output. The actual map contains these six forest models:

| Actual placed model under `o\\tree\\` | Placements |
| --- | ---: |
| `les_nw_ctver_pruhozi_t1.p3d` | 1095 |
| `les_nw_ctver_pruhozi_t2.p3d` | 1095 |
| `les_nw_jehl_t1.p3d` | 663 |
| `les_nw_jehl_t2.p3d` | 663 |
| `les_nw_jehl_trojuhelnik.p3d` | 206 |
| `les_nw_trojuhelnik.p3d` | 416 |

That is **4138 actual Nogova forest placements**. A source-only example control is original owner ID173643, `o\\tree\\les_nw_jehl_t1.p3d`, rounded world position (1626.3,132.9,10077.0); adjacent ID1513 is jehl_t2 at (1624.4,132.1,10073.2). Runtime must query actual current placement/pose rather than treating these rounded offline positions as installed proof.

All 14 `tree/les*.p3d` sources in original `O.pbo` were extracted and inspected, including source properties, visual sections and referenced textures. Twelve have visual forest geometry and explicit class=forest / map=forest square, triangle or border. Their 39 raw visual LODs are audited below and in [the independent census fixture](../../../tests/unit/engine/WgpuRenderer/forest_nogova_visuals.txt). The remaining `les_nw_ctver_mlaz` has **zero vertices/faces/sections in both visual LODs** and remains excluded. `les_singlestrom` declares map=tree, not forest; it retains the separate individual-tree path and does not inherit forest proof. No unexamined O-family variant is silently labelled supported.

| Exact forest model | Raw visual LOD / original resolution |
| --- | --- |
| `o\tree\les ctverec geom.p3d` | 0/1, 1/7 |
| `o\tree\les_nw_ctver_pruhozi_t1.p3d` | 0/1, 1/7, 2/10 |
| `o\tree\les_nw_ctver_pruhozi_t2.p3d` | 0/1, 1/7, 2/10 |
| `o\tree\les_nw_ctver_pruhozi.p3d` | 0/1, 1/7 |
| `o\tree\les_nw_ctver.p3d` | 0/3, 1/16, 2/40 |
| `o\tree\les_nw_jehl_ctver_pruhozi.p3d` | 0/2, 1/6, 2/12, 3/40 |
| `o\tree\les_nw_jehl_ctver.p3d` | 0/3, 1/10, 2/20, 3/40 |
| `o\tree\les_nw_jehl_mlaz.p3d` | 0/3, 1/15, 2/40 |
| `o\tree\les_nw_jehl_t1.p3d` | 0/2, 1/6, 2/12, 3/40 |
| `o\tree\les_nw_jehl_t2.p3d` | 0/2, 1/6, 2/12, 3/40 |
| `o\tree\les_nw_jehl_trojuhelnik.p3d` | 0/2, 1/6, 2/12, 3/40 |
| `o\tree\les_nw_trojuhelnik.p3d` | 0/1, 1/7, 2/10 |

Combined with Data3D: **25 models, 84 raw visual LODs, 57 unique referenced textures, 223 admissible foliage tuples and 50 explicit bark/bare-skeleton tuples**. All newly referenced textures were extracted, decoded and visually inspected; existing shared Data textures reuse their previous audit. Material admission still requires the actually retained ordinary cutout and unchanged uploaded face source. A known asset in a wrong model/resolution row stays excluded.

## Material distinction

`dd_les_kura03` and `akat01_krd2` are solid bark; `kmenletokruh` is a log end. `dd_les_vetvicky` and Data `les_suche_vetvicky` are bare branch skeleton photos despite their alpha holes. Those entire textures remain zero.

The new categories follow actual photographs: `dd_lest01` upper leaf crown / lower long stems; `dd_lest03` and `dd_lesd03` paired crowns / lower long trunks; `dd_lest02`, `dd_lesd02` and `dd_les_vetve04` foliage-only photographed branches. `dd_bush06` has a fully bare LEFT half, excluded explicitly; only its right leaf-bearing crop can coat. `afn_strom_04_new2` separates left leaf sprays from the large right tree's upper crown and exposed long trunk. `smrcicicek` excludes the bare upper shoot and low stem while admitting its middle leaf-bearing branches. Other leaf/grass/shrub photographs preserve conservative low-stem crops.

The far `dd_les_lod` is a **512px cutout, 62.9% clear / 37.1% opaque**. Crowns occupy its upper strip; long trunks occupy the middle, scattered leaf clumps the bottom. Only the upper crown crop coats. The lower clumps cannot establish upper-canopy authority and remain zero. `dd_lest01` is 71.7% clear and `dd_les_vetve04` 53.5% clear. No geometry, normal, alpha, wind or coverage-classification relaxation is introduced.

Per-fragment physical-map shelter, actual deposit/altitude, source-filtered photographed frost and conservative missing/overflow negatives remain unchanged. These masks are authored photographic regions; small visible twigs inside a crown are not separately segmented wood or reconstructed volumetric snow.

## Loader compaction correction

Raw source LOD indices are **not always loaded slot indices**. The actual engine `OptimizeShapes` deletes an overcomplex prefix and VDecal/unsupported TL levels, then compacts the surviving slots. The existing offline engine reader on original `les_nw_jehl_t1` reports seven loaded levels: visual resolutions2/6/12, then Geometry at slot3. Raw inspection has an extra visual resolution40 before Geometry; that far VDecal was removed. The raw source count84 is therefore not a claim of84 simultaneously loaded/rendered levels.

Admission now finds the **unique audited original resolution**, checks the actual retained slot can only have shifted left, and selects that original texture-membership row. Duplicate/unknown/nonfinite resolutions fail closed. Owner proof checks actual loaded normal levels; each actual selected section additionally requires no proxies. This preserves exact source semantics under ordinary loader compaction without reinstating any removed LOD or guessing a new layout.

The standalone CPU test compares both independent source fixtures against all57 known textures at every original row, and verifies all possible left-compacted slots keep the correct material row. An impossible rightward shift or ambiguous resolution remains rejected. Texture membership uses a private CPU uint64 bitset so indices above31 are preserved; the existing GPU section flags/layout remain unchanged.

## Verification and remaining gates

CPU exact-policy checks PASS:25models/84rawLODs/223foliage/50bark-or-skeleton negatives, compacted-slot equivalence, impossible reorder/duplicate/nonfinite resolution and ordinary ownership/material negatives. Existing near-atlas CPU rectangle bounds/negatives still PASS. Actual production-helper GPU probe expanded to64 cases, including each Nogova category, lower trunk/leaf-clump negatives, bare left bush/upper shoot, invalid map/shelter and global ablation; **not run by this source-only author**.

Root must run real shader composition/numeric checks and full build, then inspect matched installed medium/far/top views of actual Noe forest owners alongside original-map Data3D controls. Require selected-material and owner trace, active snow/map ready/no-overflow, stable installed hashes and normal exit. Inspect white textured crowns from above, retained branch texture/alpha holes, dry long trunks, no solid white wedges and partial overhead roofs. Include normal LOD transitions and missing-map/snow-off negatives. Costs and all-map appearance remain unmeasured until those installed controls.

