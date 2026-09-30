# Exact Nogova cultivated-soil contact admission

The owner is testing original Nogova and requests real volumetric mud imprints. Universal ground puddles are intentionally broader than soft-soil contact admission: asphalt, rock and roofs do not become deformable mud because they receive a cosmetic water film.

## Retail source evidence

Read-only `PoseidonTools` extraction of the installed retail `AddOns/Noe.pbo` and `AddOns/O.pbo`, config debinarisation, WRP inspection and PAA decoding establish this bounded exception. No game or GPU was launched for this audit.

Noe's config **does contain CfgSurfaces**: grass (`t1??????`, `b1??????`, `trava2`, `blita2`, `blita3`), sand (`pt??????`), rock (`l1??????`, `lom2`), snow (`n1*`, `snih*`) and hard road/runway families. It contains no surface entry for `pole1` or `pole2`; the base `Default/files=default/soundEnviron=normalExt` with no clutter character is their fallback. This corrects any earlier statement treating every Nogova texture family as Default. `PatternMatchQM6` explicitly permits the trailing six `?` to match zero characters, so plain `b1`, `t1`, `pt` and `l1` are recognised.

The actual Noe palette references `o/pole1.paa` at index 8 (12 cells) and `o/pole2.paa` at index 12 (158 cells). Decoded retail images depict cultivated bare soil texture with a green grass border, rather than a uniform grass or hard surface. Admission is based on these exact authored field-source identities and their visible cultivated structure, not average brown colour or a guessed Czech-name substring. `pole3.paa` also exists in O.pbo but is not in the retail Noe palette and is not admitted by this change.

Source hashes (SHA-256):

| Retail source | SHA-256 |
| --- | --- |
| Noe.wrp | `9b27b11661d3cb30f9f0749a9a4616bfc58d723c4750666d1e674dd4213598d2` |
| O.pbo/pole1.paa | `02fb8b71b91b6ada3b845b512c2eb5c6521240952cd516e26a068ec16d95b0d0` |
| O.pbo/pole2.paa | `dab8594c22572ffa7201ec2f2911f9307c8081ceeb7635f8c99e81e7676b4c0e` |

Decoded inspection images and extracted data remain in this agent's ignored `build/terrain-puddle-policy/nogova/o-soil` directory; retail assets are not copied into tracked source. These hashes identify inspected evidence, not runtime content-hash enforcement against mods replacing the same virtual source path.

## Contact policy

Keep the existing exact Everon `Field/pol/dirt/empty-character` admission. Add only exact world `noe/noe.wrp` plus exact virtual texture `o/pole1.paa` or `o/pole2.paa`, unchanged Default fallback metadata and empty clutter character. Normalise path case/slashes and one optional virtual root slash; arbitrary basename aliases or other directories/maps are excluded. No runtime config search or texture decoding is introduced.

Each of the existing nine entire-boot support probes must lie within source tile UV .12–.88 in both axes, safely inside the cultivated centre and outside the green border. A boot centre at an admission edge fails its outer support probes. Texture identity is read from the actual loaded cell's Texture, while SurfaceAt supplies the selected point quadrant metadata. Native/A3 source mixtures remain conservatively excluded until exact point-mask proof is available. The existing contact-height, slope, sea, snow, road-object and upward view-roof collision gates all remain in force.

Accepted `MUD_STEP` trace rows now report actual surface, files, sound and texture instead of hardcoding Field/pol/dirt. The mud field's signed depression, persistence, caps, snapshots and renderer geometry pathway are unchanged by this source-admission change.

## Reproduction and bounded runtime candidates

Use `PoseidonTools terrain export-rvw4 <Noe.wrp> <Noe.rvw4>` then `python scripts/Audit-NogovaMudSources.py <Noe.wrp> <Noe.rvw4> <report.json>` for the exact texture census and coarse flat candidates. Exported RVW4 heights are quantized to .045m, with the original 50m cell spacing.

| Source candidate | Cell | Texture | Retail corner heights |
| --- | --- | --- | --- |
| X4975, Z4675 | 99,93 | o/pole2.paa | 21.465–21.645m |
| X10375, Z4025 | 207,80 | o/pole2.paa | 123.975m, all four corners |
| X10525, Z1675 | 210,33 | o/pole1.paa | 45m, all four corners |

These are central source-soil candidates, **not verified runtime poses**. Root must query the installed terrain height and check actual cell texture, road/shelter support and contact trace before declaring successful imprints. Prefer X4975/Z4675 for an open-field fixture; do not set player Y from this coarse table. Runtime acceptance needs actual player footsteps in wet soil producing negative geometry and support-height change, with dry-soil, road/grass/border and snow/roof controls. A scripted direct store stamp alone does not prove actual contact admission.

## Verification status

`scripts/Test-MudField.ps1` passes the existing signed geometry/persistence/snapshot tests plus exact world/texture/metadata matching, green-border/whole-boot exclusion, malformed/nonfinite UV rejection and hard-source negatives. The read-only source census reproduces 12+158 retail cells. `git diff --check` passes. No full game build, deployment, game/GPU run or installed appearance acceptance was performed by this agent; those remain with root.
