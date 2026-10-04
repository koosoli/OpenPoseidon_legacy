# Original CWA character model census, 2026-10-04

Source base: `3427b5e2`. Installed build at audit: `bdba184a`.
Read-only inspection; no game launch, installed changes, config or retail asset
commit. The companion JSON contains class names, resolved inheritance/model/moves
bindings and aggregate source evidence only.

## Complete denominator

The installed base `BIN/CONFIG.BIN` and every root config in installed `AddOns`
were inspected, retaining explicit parents through debinarization and resolving
the inherited `Man` classes. There are 102 class definitions with inherited
character models: 77 scope2 classes, 22 scope1 classes and 3 scope0 templates.

**37 present class-backed character models** comprise 35 editor-public models
plus two story-only models, `Angelina` and `GeneralE` (Guba). Scope1 story models
must be attempted in a runtime spawn test; their absence from the editor is not
a reason to drop them from the character denominator.

There are also exactly **37 present model + moves-binding families**. Every
present model resolves to a single moves binding across its classes: 32 use
`CfgMovesMC`, and the five Woman models use `CfgMovesMCWoman`, which is explicitly
declared as inheriting `CfgMovesMC` by the original `O.pbo` config. The JSON
records each individual class binding, including fake, night, handgun, medic,
crew, pilot, resistance and weapon variants. This supports one representative
per unique model/rig for geometric coverage; class and weapon variants can
still require separate behavioral checks.

The three nonpublic template classes `Man`, `Soldier`, `SoldierLAW` reference
`data3d\vojakw.p3d` with `NoDefaultMoves`. That member is absent locally and has
no scope2 descendant retaining the model; it is excluded from the present
character denominator. `DTA/Data3D.pbo` also contains the unreferenced legacy
`mc civil2stary.p3d`: no surveyed stock class points to it, so it has no original
spawn representative and is recorded separately rather than silently counted
as an editor character. Its source was also decoded: 11 LODs, at most 1,520
points, all 25 nonempty LOD0 body/equipment selections, 14 Fire components;
this establishes asset presence and bounded source structure, not admission.

## Model representatives

Scope2 is editor-public; scope1 identifies the two story-only models. Public
classes counts include inherited variants. Addon rows require their original
addon's config registration and installed model archive; distributing new
retail assets is not needed. Resistance sniper requires `O_WP.PBO`; Camel
pilots require `BISCamel.pbo`; expansion civilians and women require `O.pbo`.

| Model | Representative spawn class | Scope | Public classes | Required registration | Positive shared anchors |
|---|---|---:|---:|---|---:|
| `biscamel\biscamelpilot.p3d` | `BISCamelPilot` | 2 | 1 | BISCamel.pbo | 13/15 |
| `biscamel\biscamelpilot2.p3d` | `BISCamelPilot2` | 2 | 1 | BISCamel.pbo | 13/15 |
| `data3d\angelina.p3d` | `Angelina` | 1 | 0 | Base config | 10/15 |
| `data3d\mc civil.p3d` | `Civilian` | 2 | 1 | Base config | 15/15 |
| `data3d\mc civil2.p3d` | `Civilian2` | 2 | 1 | Base config | 15/15 |
| `data3d\mc civil3.p3d` | `Civilian3` | 2 | 1 | Base config | 15/15 |
| `data3d\mc crewg.p3d` | `SoldierGCrew` | 2 | 1 | Base config | 15/15 |
| `data3d\mc pilote2.p3d` | `SoldierEPilot` | 2 | 1 | Base config | 15/15 |
| `data3d\mc pilotw2.p3d` | `SoldierWPilot` | 2 | 2 | Base config | 15/15 |
| `data3d\mc saboteur.p3d` | `SoldierWSaboteur` | 1 | 3 | Base config | 15/15 |
| `data3d\mc saboteurday.p3d` | `SoldierWSaboteurDay` | 2 | 3 | Base config | 15/15 |
| `data3d\mc snipere2.p3d` | `SoldierESniper` | 2 | 1 | Base config | 15/15 |
| `data3d\mc sniperw2.p3d` | `SoldierWSniper` | 2 | 1 | Base config | 15/15 |
| `data3d\mc specnas2.p3d` | `SoldierESaboteur` | 1 | 3 | Base config | 15/15 |
| `data3d\mc tankistae2.p3d` | `SoldierECrew` | 2 | 1 | Base config | 15/15 |
| `data3d\mc tankistaw2.p3d` | `SoldierWCrew` | 2 | 1 | Base config | 15/15 |
| `data3d\mc vojake2.p3d` | `SoldierEB` | 2 | 9 | Base config | 15/15 |
| `data3d\mc vojake2_guba.p3d` | `GeneralE` | 1 | 0 | Base config | 15/15 |
| `data3d\mc vojakeo2.p3d` | `OfficerE` | 2 | 3 | Base config | 15/15 |
| `data3d\mc vojakg2.p3d` | `SoldierGB` | 2 | 7 | Base config | 15/15 |
| `data3d\mc vojakg3.p3d` | `SoldierGMG` | 2 | 2 | Base config | 15/15 |
| `data3d\mc vojakgo2.p3d` | `OfficerG` | 2 | 3 | Base config | 15/15 |
| `data3d\mc vojakw2.p3d` | `SoldierWB` | 2 | 14 | Base config | 15/15 |
| `data3d\mc vojakwo2.p3d` | `OfficerW` | 2 | 3 | Base config | 15/15 |
| `o\char\char01.p3d` | `Civilian4` | 2 | 1 | O.pbo | 11/15 |
| `o\char\char02.p3d` | `Civilian5` | 2 | 1 | O.pbo | 13/15 |
| `o\char\char03.p3d` | `Civilian6` | 2 | 1 | O.pbo | 13/15 |
| `o\char\char04.p3d` | `Civilian8` | 2 | 2 | O.pbo | 13/15 |
| `o\char\char05.p3d` | `Civilian7` | 2 | 1 | O.pbo | 11/15 |
| `o\char\char06.p3d` | `Civilian9` | 2 | 1 | O.pbo | 11/15 |
| `o\char\char07.p3d` | `Civilian10` | 2 | 1 | O.pbo | 11/15 |
| `o\char\civilistka01a.p3d` | `Woman1` | 2 | 1 | O.pbo | 11/15 |
| `o\char\civilistka01b.p3d` | `Woman2` | 2 | 1 | O.pbo | 11/15 |
| `o\char\civilistka02a.p3d` | `Woman3` | 2 | 1 | O.pbo | 11/15 |
| `o\char\civilistka02b.p3d` | `Woman4` | 2 | 1 | O.pbo | 11/15 |
| `o\char\civilistka02c.p3d` | `Woman5` | 2 | 1 | O.pbo | 11/15 |
| `o_wp\mc_specg.p3d` | `SoldierGSniper` | 2 | 1 | O_WP.PBO | 15/15 |

## Compatibility facts and limits

All 37 models are present, parse as original ODOL7, and have finite points.
They have 9-12 LODs and at most 2,074 points in any LOD, below the current 4,096
point bound. All 37 have 14 authored FireGeometry components; the first 12
components each have exclusive full weight to one stock body bone.

The verified shared-anchor distribution is 22 models with all 15, five models
with13, nine models with11, and Angelina with10. The full15 set consists of
21 class-backed Data3D `mc ` models and the O_WP sniper. The two Camel pilots
and O male civilian `char02/03/04` lack only toe overlaps. O male civilian
`char01/05/06/07` and all five women also lack shin/foot overlaps. All those
models retain nonempty feet and actual memory foot points. Alternate measured
foot geometry is recorded as evidence, not admitted joint positions.

Angelina lacks positive neck/head, both shoulder/upper-arm and both thigh/shin
intersections in every graphical LOD (0,1,2), not merely the highest LOD.
Actual neutral FireGeometry vertex pairs for those five missing boundaries
have gaps of .1005m (neck/head), .0610/.0603m (shoulders) and .0102m (knees),
within a bounded .12m paired-surface check. Those measured points are recorded
for a possible geometry-derived spherical-joint implementation; they do not
provide an authored animation overlap or anatomical hinge axis.

Its normal animation fallback should remain until separate safe support is
implemented and tested; adding the filename to a generic allowlist does not
prove a complete articulation.

The private raw audit used the existing tracked
`scripts/physics/inspect_stock_corpse_rig.py` decoder and
`inspect_stock_corpse_parts.py`, including positive weighted intersections and
proxy-vertex exclusion. PoseidonTools independently supplied model/LOD/named
selection and simulation-source receipts. Its `model selections --bones`
reports that ODOL7 has no static bone reader; raw source receipts declare no
embedded skeleton. Consequently neither the 33-bone runtime animation palette
nor runtime proxy normalization, hinge fits, contact/settling, death admission,
or projectile wake behavior is certified by this census. Those require the
parent agent's installed-game runner and existing runtime capability gates.

Private extracted configs/models and full geometry measurements remain under
ignored `build/ofp-character-census`; they are not staged. The companion
metadata JSON provides the finite class/model list for runtime smoke testing.
