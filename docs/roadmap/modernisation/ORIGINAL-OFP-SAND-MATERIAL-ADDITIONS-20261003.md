# Original OFP sand material additions, 2026-10-03

Source-only candidate based on `4077009f`. Installed-game appearance and normal
binding verification remain required. No original game archive was modified.

## Concrete change

The visual pack previously bound its authored sand macro/detail normals only to
`eden/ps.paa`. Add exact entries for `abel/pi.paa`, `o/pt.paa`, and `o/ps.paa`.
Their **albedo remains the original source texture**; this avoids replacing
Malden's and Nogova's terrain colours with the generated Everon sand colour.
All three reuse the already packaged `sand_nohq.paa` and
`sand_detail_nohq.paa`; no new texture generation or layout is introduced.

| Source | Required original archive | Actual authored class | Census pure cells |
|---|---|---|---:|
| `abel/pi.paa` | `Dta/Abel.pbo` | SandAbel / `pi??????` / sand | 43286 |
| `o/pt.paa` | `AddOns/O.pbo` | SandDark / `pt??????` / sand | 4964 |
| `o/ps.paa` | `AddOns/O.pbo` | Sand / `ps??????` / sand | 20057 |

Counts come from `ORIGINAL-OFP-CONTACT-SOURCE-CENSUS-20261001.json`; they include
underwater, shore, steep and obstructed cells and do not establish usable
walking area. All four quadrants of these three pure palette textures have
the stated class. Nogova's mixed `ps/pt` PAC palettes remain outside this
appearance addition. There is no wildcard or whole-map override.

## Loader and scale contract

`TextureBankWgpu::ResolveLegacyMaterial` looks up the exact normalized source
path and requires the loader-selected archive's weakly canonical path to equal
the manifest's original archive. A user mod that supplies the same virtual
texture name from another archive retains precedence. `Load` retains the
original `SetName(name)` and cache key, sets `_sourceName` to the explicit
albedo path, and initializes that path directly through the texture source
factory. When the two names are equal, this opens the existing original PAA;
it does not recursively call TextureBank to resolve another replacement.
The existing failed-source fallback and half-material normal guard remain.

The macro normal uses the existing 50 m tile specification and original tile
UVs. The detail remains a 0.5 m periodic tile, or two repeats per world metre.
`sample_legacy_normal` samples alpha/green NOHQ with explicit gradients,
absolute world coordinates, and its existing 15–35 m detail fade. Thus the
addition shares the established texture cache, derivative filtering and
distant-fetch avoidance. Those source properties are not proof of visual
acceptance on the newly covered maps.

No SurfaceAt, CfgSurfaces, Mud/Sand admission, retained water, contact geometry,
or weather eligibility code changes. These added normals also do not create
volumetric sand depressions; the current physical sand path owns that behavior.

## Actual installed source audit

Existing PoseidonTools performed filtered extraction and configuration decoding
only. Outputs are in ignored `build/soft-source-audit`. Base CONFIG.BIN SHA256
still matches the checked-in census:
`DA9B790900357450D06CB27FD0CDBCF671E6316D6A6D4D0C9982D358AFF703CA`.
The actual Noe config explicitly supplies SandDark / `pt??????` / sand;
base configuration supplies Sand and SandAbel.

| Extracted original member | SHA256 |
|---|---|
| Abel `pi.paa` | `EB63E112621D246183CC0809C8C1B6AC2635520A75B970FED83DD0C4DFB49B7E` |
| O `pt.paa` | `C05B92DA8F61EF37783A8352B6CBED2011EA5546D2C1B276B75170B4718EA733` |
| O `ps.paa` | `002656F3F5A70090250AB2BD7F5BEBD55E0BE66092B801F483D72E618FAE324E` |
| Noe `config.bin` | `CCBE4A178B65D83F3C55E9BE63579B127FC2967D30C188BE490DE295A7FA9D9E` |

Original archive hashes: Abel
`034FB6E8A27EA004C07308AC1A7ED6A9F8ED7E8264B8B3C2658372E72A286429`, O
`E3111E081AFED428B67896CE11F374F591417990D2313F437EBCE077B9986E23`, Noe
`C98A6C97F21264E20F91E2E7EE024A068A11821361266041AC0C47DE4F05CF72`.
The installed visual PBO was
`6B0527CE8A1A320D853198BCBAC2E7A2A8AFC6A3ED26CBAF6E1D75987B6BB558`;
its extracted manifest had only the original three road/Everon/gravel entries.

## Explicit remaining all-map gaps

Cain/Kolgujev resolves all 65536 census cells to Default. The actual Cain
texture archive lists 454 members and no config. Installed base CfgSurfaces
and Noe's addon do not define its `l4`, `j9`, `t9`, `u2`, or `k5` families.
Prior extracted texture review identifies vegetation/rock among these families;
the granular `l4` image cannot establish sand rather than gravel. No true
authored alias was found. Abel has no source-admitted soil family either.
Their gaps cannot be closed by forcing wetness or interpreting texture colour.
This does not prevent physical rainwater or general ground wetness on them.

The literal configuration audit also found unrelated base Beton
`files="kameny.pac"`: `PureTextureName` strips the extension before the literal
case-sensitive pattern test, so that rule cannot match basename `kameny`.
It does not explain Cain's two-character terrain defaults or license a soft
surface classification. No lookup or hard-surface edit is included here.
The existing trailing-six-question-mark compatibility already correctly
matches `pi`, `ps`, and `pt`; there is no demonstrated bug there.

## Validation and runtime follow-up

`scripts/default-content/test_materials.py`: 11 tests PASS with the bundled
Python. New checks prove the entries target census-authored pure sand, retain
original albedo and exact archives, share packaged assets and physical scale,
and exclude unknown/rock/mixed palettes. Static loader contract checks cover
source/cache identity, original archive selection and failed-source fallback;
they do not execute the C++ loader. Existing generated-normal bounds,
determinism, encoding, ripple and grain tests remain passing. `git diff --check`
passes. No device/full build, content packaging, install or game was run here.

Root must package/deploy with the ordinary lock and provenance workflow, then
verify logged macro/detail bindings on actual original Abel pi and Noe pt/ps,
retained albedo, near/mid/far grain stability, and no normal seam or distant
glitter regression. The normal/detail OFF arms and an overriding mod source
must retain their intended behavior. Existing actual contact and sand pit
receipts are separate from these appearance checks.
