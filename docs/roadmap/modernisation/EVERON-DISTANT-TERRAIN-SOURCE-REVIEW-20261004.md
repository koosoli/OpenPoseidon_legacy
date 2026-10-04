# Original Everon: distant terrain source review

The floating-house report remains **unreproduced and unidentified**. This read-only follow-up found a source mechanism that can create distant foundation gaps, but its two distant source controls are hidden behind substantial native hills from the owner's reported camera. A paired capture aimed at those controls would not establish the reported defect, so no script expansion or production placement/rendering change was made.

## Evidence and scope

The installed [placement diagnostic](EVERON-PLACEMENT-DIAGNOSTIC-20261004.md) remains authoritative for its actual owner/native observations and root-inspected images. This follow-up uses its final `087ded` receipt, installed `f6e5c1d5`, with unchanged original `Worlds/eden.wrp` SHA-256 `5a2550b27612fc4c8ccdec55ce2555c1ed23114d46f8b687ce5fb8b273515a79`. The [source receipt](EVERON-DISTANT-TERRAIN-SOURCE-RECEIPT-20261004.json) pins the inspected root source, file hashes, original-source hash, subdivision cache hash, all candidate results and numeric scope limits.

The v2/L3 original-Everon cache has 2048² float heights at 6.25m. Its stored source FNV agrees with the full original WRP height data and dimensions. The cache's fine two-triangle surface reproduces **all 198 actual saved foundation SurfaceY observations** within 0.000047m. This source-bound agreement supports the following offline calculations; it is not a GPU height readback or current submitted-frame proof.

For each saved bottom sample, the analysis evaluated the cached heightfield on fixed 6.25,12.5,25,50,100 and200m anti-diagonal triangle lattices, then subtracted that hypothetical support height from the actual owner-transformed sample Y. It uses double arithmetic for the offline interpolant. These are lattice endpoints, not an exact intermediate vertex-morph replay. Native support agreement includes the actual query's float-rounded XZ.

## What the source does

`TerrainWgpu::BuildQuadtree` and `SelectCdlod` select terrain patches by camera-to-node-AABB distance. With default `WGR_TERRAIN_LOD_BASE=4`, ratio 2 and the 6.25m/32-cell leaf, distance ranges are 800,1600,3200,6400m and so forth. Default morph fraction .5 blends each level toward its parent lattice over the latter half of the interval between adjacent ranges. `terrain.wgsl::vs_terrain` moves vertex XZ toward the coarser lattice and samples height there; rasterised triangle interiors therefore need not coincide with the full native SurfaceY at the same XZ. A rigid house can preserve its correct authored/native transform while coarse ground under it appears lower.

The ordinary object shaders retain rigid model transforms. ClipLandOn road/decal vertices have a separate `surface_y_raster` route; ClipLandKeep vegetation intentionally retains fine-ground support. That helper is an approximation of CDLOD selection/morph and is not a general house-placement fix or exact submitted terrain readback. Moving every building to its result would also endanger authored elevations and bridges.

The startup/read-only harness exposes native heights and object frames. The existing WGPU frame hash can report terrain-node count/hash, but no found API returns selected node coordinates/morph bands or a GPU rendered-terrain-height value. The earlier diagnostic did not record inherited CDLOD environment overrides or an actual camera FOV, so its default-selection status and screen admission cannot be asserted from this source analysis.

## Source controls and obstruction

Positions use engine **X,Y,Z**; camera commands use **X,Z,Y,azimuth,elevation**. Reported camera: `[5805.63,46.35,3597.97]`, az147.8°, el6.3°.

| Authored ID/model | Actual position X,Y,Z | Distance | Aim at origin | Source visual LOD resolutions |
|---|---|---|---|---|
|25191, `data3d\ryb_domek.p3d` |10319.622070,33.098419,332.722534 |5571.173m horizontal |az125.880524°, el−0.136283° |0.75,1.5,2.5,4,6 |
|31997, `data3d\helfenburk_budova2.p3d` |9385.744141,193.202118,1031.389648 |4405.060m horizontal |az125.636702°, el1.909371° |1.5,3,5,7,9 |

They are about 21.92°/22.16° left of the reported azimuth; without actual FOV that does not certify whether they fall on screen. All source visual LODs share their respective sampled minimum local foundation Y:−3.055555m and−6.263978m. These are source LODs, not evidence of a selected pixel LOD.

- **Sampled village IDs6034–6037,6042–6045,6047 and house7710:** sampled foundations remain below the hypothetical6.25/12.5/25/50m lattices. This analysis does not support coarse ground as their reported cause.
- **25191:** sampled maximum native gap is−0.823m and remains negative on a50m lattice. A hypothetical100m lattice yields a maximum gap of+5.225m. A source-derived default level3→parent morph can make this mechanism relevant at about 5.57km, but the actual node/morph and pixels are unproved.
- **31997:** sampled native gap is−3.409m. Its100m lattice remains buried by at least 1.110m; a200m control yields+2.401m. That deliberately coarser endpoint is not claimed to be its current/default selection.

Critically, 999 interior native-cache samples along each reported-camera-to-actual-source-visual-roof line find large obstructions:

| ID | Obstruction sample X,Y,Z on sightline | Cached native ground Y | Ground above line |
|---|---|---|---|
|25191 |8536.595203,40.181405,1622.495283 |299.991749m |259.810344m |
|31997 |8540.837203,163.330697,1637.102611 |305.316161m |141.985464m |

These are discrete source obstruction witnesses, not a continuous GPU ray or object-occlusion result. They nevertheless make a reported-position comparison of these two controls unsuitable for identifying a foundation gap. The earlier root-inspected village close views remain grounded; no visible nearby candidate with a supported gap was found in this bounded pass.

## Conditional next diagnostic

First identify the owner's actual visible house or a reproducible visible foundation gap. Only then use one bounded installed terrain-detail control at the same camera/source/owner transform. Existing `WGR_TERRAIN_LOD_BASE=64`, with ratio 2/morph.5, gives a level0 range 12800m and an unmorphed fine interval out to 6400m; it can isolate terrain presentation while leaving object transforms and the object LOD governor untouched. This was **not run**, is potentially expensive and is not a production setting or performance recommendation.

Such a diagnostic must record the original `WGR_TERRAIN_LOD_BASE`, `WGR_TERRAIN_LOD_RATIO`, `WGR_TERRAIN_MORPH` and `WGR_TERRAIN_EXTENT`, exact returned camera, unchanged source/height revision and owner frames, matched installed EXE/DLL and normal zero exit. If the target remains occluded or unidentified, preserve the unresolved result. No global ground snap, source WRP edit or automatic sinking is justified by this evidence.
