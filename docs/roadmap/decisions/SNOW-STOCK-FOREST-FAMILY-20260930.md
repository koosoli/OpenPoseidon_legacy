# Stock forest foliage and canopy-top source candidate

Status: **SOURCE CANDIDATE; expanded actual GPU probe, full build and installed appearance pending.** Supersedes the two-tuple restriction in [the original audit](SNOW-STOCK-FAR-FOREST-ATLAS-20260930.md). The owner's Everon screenshot rejects the earlier flat white billboard result. Alpha/crown coverage alone is not visual acceptance.

## Audited source family

Offline extraction of installed CWA `DTA/Data3D.pbo`, followed by the existing `PoseidonTools model inspect --all`, simulation-source and loaded-engine UV diagnostics, establishes 13 models / 45 visual LODs / 29 unique referenced textures. All visual named-selection counts are zero; collision components are not per-tree visual identities. All 29 face textures were extracted from `Data.pbo` or `Merged.pbo`, decoded with the existing tool and inspected. The independently stored full texture census is [forest_stock_visuals.txt](../../../tests/unit/engine/WgpuRenderer/forest_stock_visuals.txt).

| Exact model | Visual LOD / original resolution |
| --- | --- |
| `data3d\les ctverec dark.p3d` | 0/3, 1/20 |
| `data3d\les ctverec mlazi.p3d` | 0/3, 1/15, 2/40 |
| `data3d\les ctverec pruchozi_t1.p3d` | 0/2, 1/6, 2/12 |
| `data3d\les ctverec pruchozi_t2.p3d` | 0/2, 1/6, 2/12 |
| `data3d\les ctverec.p3d` | 0/3, 1/10, 2/20, 3/40 |
| `data3d\les trojuhelnik pruchozi.p3d` | 0/2, 1/6, 2/12, 3/40 |
| `data3d\les trojuhelnik.p3d` | 0/3, 1/16, 2/30, 3/60 |
| `data3d\les_su_ctver_mlaz.p3d` | 0/3, 1/15, 2/40 |
| `data3d\les_su_ctver_pruhozi_t1.p3d` | 0/1, 1/7, 2/16, 3/40 |
| `data3d\les_su_ctver_pruhozi_t2.p3d` | 0/1, 1/7, 2/16, 3/40 |
| `data3d\les_su_ctver_pruhozi.p3d` | 0/1, 1/7, 2/16, 3/40 |
| `data3d\les_su_ctver.p3d` | 0/3, 1/16, 2/40 |
| `data3d\les_su_trojuhelnik.p3d` | 0/1, 1/7, 2/16, 3/40 |

The policy admits **108 exact foliage tuples** and explicitly rejects **21 referenced bark tuples**. The five standalone bark images are `kmen1_les`, `kmen2_les`, `kmen_borovice`, `lesni_kmen_smrku` and `n_strom_01_kura`; none obtains snow via forest admission. A known texture used at the wrong model/LOD/resolution remains rejected.

This is the Data3D family used by the original forest system, not all OFP-map completeness. Offline `O.pbo` inventory reveals a separate `o\\tree\\les_nw_*` Nogova family, now queued for its own actual placement/texture/geometry audit. Native forests, uninspected models, arbitrary subclasses and direct-only sections still have no forest snow proof.

## Material masks and preserved rendering

Private existing section bits 16..23 carry nine categories; no public ABI, buffer, vertex format, extra varying or crown-centre encoding changes. The retained section must already be an ordinary cutout, retain its actual uploaded face albedo, have no maps/RVMAT/conflicting flags, and have no proxies at that actual selected level. Only exact primary/static/intact ForestPlain/Forest owners with loaded audited visual levels and conform modes 0/1 receive proof. Unknown classes, unowned parts and unsupported levels fail closed.

The near CWA `merged\\00001&krovi4.paa` mask admits photographed foliage rectangles while excluding the bare branch skeleton, lower-right bark/log tiles and unused grey regions. The Abel `merged\\00011&str_fikovnik.paa` mask admits actual crown photos, its foliage-only tall card and canopy-top quadrant; the narrow bark strip and all bottom root/bark/blank tiles remain zero. Each rectangle has a four-texel inward feather at 1024px to protect neighbouring tiles. Row selection bounds evaluation to the relevant rectangles.

Forest-strip categories keep broad crown/trunk transitions. `les_dark_new` lower ground tiles are excluded, and `lesabelnew` has separate authored crown-row/stem masks and its lower foliage-wall region. Standalone photographed conifer branches and `lesabeltop` use foliage-only admission. Shrub photos protect their low stems. Small twigs photographed inside a foliage rectangle are material relief, not separately segmented wood; unsupported lower regions intentionally stay bare.

Actual `lesabeltop.pac` is DXT1 cutout: **15.6% clear, 84.4% opaque texels**. Its source sections are IsTransparent/NoClamp; no opaque-route relaxation is required to whiten canopy tops. The loaded far `les_su_ctver` has horizontal canopy-top vertices at model Y=4.86483 across a 50m square, independently of lower wall vertices. Top coverage uses actual alpha-tested geometry, not an invented cluster cap.

The actual sampler addressing is essential: the triangular passable far strip has U=-.994503..1.99601; the Abel canopy also repeats. The shader maps UVs using the existing sampler bits (clamp U=1, clamp V=2), matching the sampled photograph. Clamped near atlases cannot wrap into unrelated atlas regions. The point-filter bit leaves the same addressing.

Actual deposit and smooth altitude snow drive coverage. Only the dedicated physical weather map provides per-fragment exposure; missing/pending/outside/border/overflow stays bare. There is no CPU whole-cluster exposure lease or artistic openness fallback. Stock maps' own upper geometry and partial external roofs retain that map's existing shelter semantics.

The luminance-preserving frost from e4557548 retains filtered photographed leaf/branch contrast at complete snow coverage, avoiding a constant-white polygon fill. It adds only broad filtered diffuse mottling. This change preserves alpha/A2C/discard/depth, wind, geometry, source normals and the existing subpixel filtering; it adds no snow specular dots.

## Verification and remaining gates

Standalone CPU policy PASS: exact 13-model/45-LOD independent census; 108 admitted tuples; 21 bark negatives; all 29 known textures checked at every audited LOD; wrong assets/resolutions/material gates/class/conform/loaded ownership rejected. CPU actual-source rectangle algebra PASS: 132098 finite bounded samples plus explicit crown/top positives and bark/log/ground/exposed-trunk negatives. Existing powder/frost CPU algebra still passes. These are CPU checks, not WGSL execution.

The actual production-helper GPU probe now has 48 samples in enabled/disabled arms: all categories, near atlas bark/trunk/ground negatives, canopy top, repeating/clamped UVs across all eight actual sampler indices, nonfinite inputs, unpublished map and closed shelter. The earlier expectation was corrected in 208e27b3 to evaluate the production smoothstep exposure rather than raw reach. **The expansion's GPU test has not been run by this source-only author.**

Root must run consumer composition and numeric GPU probes, compile the full engine, then matched installed snow/atlas ON/OFF captures with actual owner/material trace, stable installed pair hashes, normal exit and weather-map ready/no-overflow proof. Inspect actual forest near/mid/far/top views and LOD transitions on Eden/Abel/Cain, with bark/ground/alpha negatives and a partial overhead roof control. Nogova requires the separately audited O family. The owner specifically requires textured snowy crowns and white forests from above; simply proving positive coverage is insufficient.

No measured runtime cost or universal map/mod compatibility is claimed. Registration uses finite static tables; owner checks reject nonforest map types before model lookup. Fragment arithmetic is fixed and there is no new pass/ray scan. Same-path altered stock assets are not byte-certified by runtime policy.

