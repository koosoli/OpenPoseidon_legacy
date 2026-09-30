# Original CWA cultivated-soil admission, 2026-09-30

## Bounded change

Original retail Everon has no terrain surface class named Mud, Dirt, Soil or
Clay. Its bare cultivated soil is authored as `CfgSurfaces/Field` with
`files="pol"`, `soundEnviron="dirt"`, roughness 0.15 and dust 0.3. The installed
`eden/pol.paa` image is predominantly brown cultivated soil with small green
edging; both root and the source-audit agent inspected its decoded 128x128 image.

Admit this **exact three-part legacy identity**: class `Field`, file pattern
`pol`, sound environment `dirt`. Field name alone, dirt sound alone, translated
texture-name guesses, altered patterns or missing/conflicting metadata remain
insufficient. Each of the existing four terrain texture quadrants must still
qualify; mixed tiles remain dry. Native semantic and authored A3 source-slot
classifiers are unchanged. The predicate runs only in material setup, reading
already cached `SurfaceInfo` strings, with no per-frame configuration queries.

`Village/files=bah` has `normalExt` metadata and remains excluded.
`MudBuilding/files=hlinasterk*` has `rock` metadata and remains excluded.
Grass, forest, sand, unknown, road, runway and mixed source rules remain intact.

## Actual retail coverage

Existing PoseidonTools `terrain textures` reads all 241 actual OPRW 2 texture
table entries from installed `Worlds/Eden.wrp`. Resolve each basename through
the same ordered config wildcard rules as TextureBank.cpp, including the
optional trailing six question marks and four two-character quadrant split.
Only texture index 6, `eden/pol.paa`, resolves all four quadrants to Field.

| Surface | Quadrant occurrences in texture table |
|---|---:|
| Rock | 321 |
| Grass | 315 |
| Forest | 125 |
| Sand | 112 |
| GrassHigh | 35 |
| Runway | 32 |
| Default | 16 |
| Water | 4 |
| Field | 4 |

These are texture-table occurrences, not geographical areas. Read the actual
OPRW 2 geography, sound, texture, random and height arrays with the repository
stock SS decoder and verify each compressed-array checksum. The texture array
has 135 cells assigned index 6 out of 65536 cells: 337500 square metres at 50 metre
land spacing. Texture table count 241 agrees with the existing CLI. No full 3x3
pol patch exists, so the existing conservative 3x3 fixture logger cannot nominate
this map; that does not invalidate the four-quadrant predicate for a pol tile.

## Installed positive-control candidate

The flat pol cell `(128,143)` has world centre `(6425,7175)`. Its four height
nodes are all 141.8849945068 metres, so the source cell is flat, well above the
coastal exclusion and has source terrain normal Y 1.0. Its source material is
entirely Field; at the centre the adjacent-cell blend weight is 1 for this cell.
Default blend_width 0.15 leaves 17.5 metres before neighbouring flags contribute.

Reproducing the production world hash/noise gives field 0.8428 at `(6425,7175)`
and 0.8694 at `(6426,7176)`, above the full-wet 0.64 threshold. This avoids the
naturally empty pattern that invalidated the first native roof fixture.

Root should run the original CWA world in an isolated owned harness, query
`water_bathymetry` at X 6425 / Z 7175, and use a candidate overhead pose
`6425 7175 <queried-ground+5> 0 -85` (source height estimate 146.885 metres).
Use fixed dry/wet/master-off arms and unchanged gates, verify the visible pixels
are terrain, and retain source eligibility/runtime wetness provenance. The
runtime height query handles map/subdivision differences; CPU heights are not
an authority for teleporting below the actual installed surface.

There are 30 pol cell centres with source normal Y >=0.9995 and 79 with normal Y >0.995.
These are candidate flatness counts, not pixel acceptance, and do not assert
that every admitted cell or every muddy part of the original map is covered.
Fields from other maps/mods qualify only if all exact metadata statements match.

## Source provenance and reproduction

| Installed source | SHA256 |
|---|---|
| Bin/Config.bin | DA9B790900357450D06CB27FD0CDBCF671E6316D6A6D4D0C9982D358AFF703CA |
| Worlds/Eden.wrp | 5A2550B27612FC4C8CCDEC55CE2555C1ED23114D46F8B687CE5FB8B273515A79 |
| Dta/Eden.pbo | CD94EDC0F19E53EAF1CB322B087DA40E50CE549D96354C5073D15A6953867242 |
| extracted pol.paa | 09567D604A9B87DEC98C2EAC5054AE562A793BEA303975F3C5814F2F2ABD2248 |

Read-only commands using the existing built tool:

```powershell
PoseidonTools.exe terrain textures <installed>/Worlds/Eden.wrp
PoseidonTools.exe pbo extract <installed>/Dta/Eden.pbo <private-audit-dir> --filter pol.paa
PoseidonTools.exe image convert <private-audit-dir>/pol.paa <private-audit-dir>/pol.png
```

The ignored source-audit folder contains the config JSON, decoded image,
texture-table census and checksum-verified source cell candidates; stock assets
are not committed or deployed. Focused standalone CPU policy tests cover the
exact admitted tuple, every missing/altered/conflicting field, class-only/native
nonadmission, Village/MudBuilding/grass/road negatives and previous explicit mud
admission. Accumulation, pause, reset and frame-rate tests also pass. No game,
GPU, full engine build, deployment or runtime acceptance was performed by this
source-only change; root owns the required installed checks.
