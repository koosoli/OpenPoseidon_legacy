# Reforger `.emat` key-coverage audit (2026-09-16)

Source: `%TEMP%\opencode\emat-census.json` (1,828-file stratified sample;
10,312 files total) cross-checked against
`engine/Poseidon/Asset/Formats/Material/EmatMaterialAdapter.hpp`,
`MaterialChannels.hpp`, `engine/WgpuRenderer/EngineWgpu.cpp`,
`XobConvert.hpp`, `ShapeDraw.cpp`. Machine table:
`%TEMP%\opencode\emat-audit.csv` (238 distinct keys).

Caveats: the census undercounts texture-valued keys (no `OpacityMap`,
`Mask_2..4`, `Mud*Map`/`Dirt*Map`, `BCR_1`/`NMO_1` rows although real
materials name them); inheritance-resolved counts. A full-corpus recount
with values (not just presence) is still open.

## Supported (reading code, not comments)

Albedo/normal/mask plumbing: `BCRMap`, `BCR_1..4` (layer order fixed),
`NMOMap`/`GlobalNMOMap`/`NTCMap`/`NormalMap`, `MaskMap`, `EnvironmentMap`,
`Color`, `Color_1..4` (native mount only), `Diffuse` (opacity),
`UVTransform_1..4`, `UVSrcGlobalMaps` (mask on UV set 2),
`GlobalMapsUVTransform` (baked into converted face UVs),
`NormalPower`, `GeometryAOCenter/Height/Width/Intensity` (parsed; runtime
default OFF), `OpacityMap`/`AlphaTest` (cutout routing), `Cull`
(double-sided per-draw pipeline), `Enabled_2/3/4` (layer switches).

## Ignored, by phase

### Phase 1 — render state / routing, no ABI change
- `CastShadow`/`ReceiveShadow` (42 Multi) — per-material caster/receiver flags.
- `Sort` (17 Basic, 17 glass...), `SortBias` (3), `ZWrite` (5+2),
  `BlendMode` (110: authoritative blend declaration, currently inferred),
  `NoDecals` (15+1), `FrontFaceInterior` (2), `ManualCascadesShadows` (3).
- `AlphaTest` (83 Basic, 58 Decal, 8 glass) + `AlphaMul` (48+22+8): the
  authored cutout threshold/gain. Currently hardcoded 0.5. Scale of
  `AlphaTest` (1.5 on wire vs 0.23 on glass) uncalibrated — needs
  measurement before wiring into `alpha_ref`.
- `MaskUVScale_2/3/4` (158), `MaskMapUVTransform` (7), `UVSrcMaskMap` (12),
  `UVSrcGlobNormal` (105) + `GlobalNMOUVTransform` (213): global-normal
  frame (uv1 vs uv0) and mask UV scaling.
- `DetailNormalMapUVTransform` et al, `UVSrcDetailNormalMap*`,
  `MCMapUVTransform`, `BCRMap2UVTransform`, `MaskScale`.

### Phase 2 — PBR scalars (need shader lanes; `layer_colour[].a` is free,
unloved tint alpha — verify before reuse; else ABI growth like crown_ao)
- `Metalness_1..4` (~405), `Roughness_1..4` (~400-490), `MetalnessScale`
  (561: 433 Basic + 71 Multi + ...), `RoughnessScale` (243+61),
  `MetalnessScale2`/`RoughnessScale2` (2Layers) — THE rust/reflective gap.
- `DetNormal_1..4` (~350), `DetailNormalPower*` — detail normal strength.
- `OverlayWhitePower` (243) / `OverlayBlackPower` (171).
- `Dirt*` (630: color/roughness/opacity/sharpness + UV + MaskMap) and
  `Mud*` (505: color/opacity/rough/metal/detnormal + UV + masks) —
  weathering overlays; strong opacities (0.87) but masked by detail maps.
- `MaskSharpness_2/3/4` (~600), `MaskOffset_2/3/4` (315-403),
  `MaskRedScale` (55) / `MaskGreenScale` (74) / `MaskBlueScale` (147).
- `DielectricReflectance` (123+), `SpecularIBL`/`DiffuseIBL`,
  `SpecularMul`, `Specular`, `AOScale`, `CavityFromAO`, `PorosityScale`,
  `Emissive*`, `GlobalAOPower`, `UseGlobAOOnly`.
- `AO_1..4`, `MudAO`.

### Phase 3 — texture slots (need slot + bind + shader fetch: ABI growth)
- `Mask_2..4` (per-layer detail gates — prime suspect for roof/door
  striping together with the blend equation, see below).
- `MudBCRMap`/`MudNMOMap`/`MudMaskMap`, `DirtBCRMap`/`DirtMaskMap`,
  `GlobalMCRMap`, `VFXMap`, `DetailNormalMap*`, `BCRMiddleMap`/`NHOMiddleMap`
  (terrain), `MacroMapLV`, `HeightMap`.

### Phase 4 — features (own projects)
- SSS/translucency (`SSSColor*`, `Backscatterig`, `ThicknessScale` 97),
  wind (mostly wired? verify), colorize system (`EnableColorization` 38+),
  `EllipsoidAO*` + `GeometryAOOffset/Distance/Power/TopScale` (crown AO
  volume is radial-only), parallax (`ParallaxScale*`), wetness/puddles
  (`Wetness*`, `Puddle*`), camo (`MatPBRCamo`), terrain materials
  (`TerrainMaterial`, `ClutterConfig`, `PlantMat`), `FadeFaces` (57),
  `FarLODDistanceOffset/HeightOffset`, `NormalsToCamera` (32),
  `LerpToTerrainNormal`, `GBuffer*` (decal deferred keys), `Sort`,
  `DecalCategory`.

## Open blend-equation questions (with evidence, no guesses shipped)
- Mask channel -> layer mapping `r/g/b -> L2/L3/L4` rests on one
  WaterTower_02 measurement. Counter-evidence: workshop roof renders far
  darker than its baked MLOD tone (0.35 ~= tinted rust base), and the
  village door reads patchy-orange where wood is expected. Needs a second
  measurement, not a wider guess.
- Base weight rule (`1 - sum` vs other) and the Mud/Dirt overlay order are
  part of the same unknown equation.
- `AlphaTest` value scale (0.23 glass vs 1.5 wire) uncalibrated.

## Done in the 2026-09-16 session (branch `parity/foliage-lighting-001`)
Crown AO default-off; opaque-solid `.emat` routing (walls/ground);
hole-less enfa coverage blends (glass); `GlobalMapsUVTransform` baked into
face UVs (fence wire); `Cull none` double-sided per-draw (+ cutout
carve-out so culled path and CPU agree); glass out of the leaf-cards opaque
route; `Enabled_2/3/4`; blend-decision trace log
(`opaque-roughness route for ...`); face-UV census.
