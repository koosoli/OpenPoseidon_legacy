# WLD-016 — Takistan's model coverage, and the one structure that blocked it

Date: 2026-08-12
Branch: `grass-lod-dissolve-photo-cards`
World: official Operation Arrowhead `takistan.pbo` → `takistan.wrp`, OPRW revision 24

## What was measured

The Arma 3 world harness (`PoseidonTools terrain census`) refused Takistan: it
gated on `IsOprw25`, while the runtime (`Landscape::LoadOprwModern`) already reads
revision 24 as well. The census is the only tool that produces denominators, so
the generation whose coverage was least known was the one it could not measure.
Accepting both revisions is a tooling fix, not a format change.

With that gate opened, Takistan measures as:

```
placements        438,906
models referenced     534   (534 in the model table, none unreferenced)
static entities     5,122   (122 classes)
road links         13,501   over 122 segment models
surface materials   3,293   (3,292 in use)
```

Against the official corpus under `inspiration/officialarma` (58 archives,
67,417 entries), the referenced models resolved as:

| outcome | models | placements | cause |
|---|---:|---:|---|
| static | 416 | 431,859 | — |
| parse_failed | 54 | 2,989 | one cause (below) |
| missing | 64 | 4,058 | one cause (below) |

Every model that resolves is ODOL revision 49. There is no revision mix in this
world: the "54 failures" were not 54 problems.

## The 54: supported revision, one unsupported structure

All 54 failed identically — `ODOL A2 model animation tables are not implemented`,
thrown from `ReadOdol49StaticDirectory` the moment `hasAnimations` was true. They
are ordinary static meshes that merely declare a door, a gate or a light: the
Takistan house families (`house_l_*`, `house_k_*`, `house_c_*`), the minarets and
mosque walls, street lamps, the coltan mine, the oil pumps. `house_l_3_ep1` alone
is placed 354 times, `lamp_small_ep1` 317 times.

Classification: **supported revision, unsupported structure** — and the structure
was already implemented one generation later. `ReadOdol73Animations` (WLD-012)
reads exactly this block for Arma 3. The Arma 2 revisions carry the same classes
and the same two lookup tables and differ only in fields later revisions added:

- revision 56 added `animPeriod` / `initPhase` after the value pair;
- revision 55 added the hide animation's second float.

Revisions 49, 50 and 52 precede both. So the fix is a `revision` parameter on the
shared reader, not a second copy of it: **shared Real Virtuality semantic**, not an
A2/OA-specific one. The Arma 3 path is unchanged — 73 is past both gates, so every
field it read before it still reads.

This layout cannot pass silently. The animation block sits immediately before the
LOD byte-offset table, and the directory reader requires every LOD range to be
ordered and inside the file. 54 models across nine families now parse, and the
runtime log for a Takistan capture contains zero `Bad version in
LODShape::LoadOptimized`.

Result: **54 → 0.**

## The 64: CORPUS-INCOMPLETE, base Arma 2

Every unresolved path is base Arma 2 content — `ca\structures\`, `ca\misc\`,
`ca\misc2\`, `ca\misc3\`, `ca\buildings\`, `ca\buildings2\`, `ca\wheeled\`,
`ca\wheeled2\`. None carries the `_e` expansion suffix.

Searching every entry of every PBO in the supplied corpus by basename found
**0 of 64**. This is not a prefix defect, not a case defect, and not a resolver
defect: the archives are not present. The corpus supplies the Operation Arrowhead
expansion (`ALDP_A2OA_*`), its DLC (`ALDP_A2OADLC_*`) and the Arma 2 Sample Models
(`A2SM_*`, unbinarized vehicle/character sources) — but no base Arma 2 addon PBOs.

Classification: **CORPUS-INCOMPLETE**. Unblocking it needs the base Arma 2
licensed data package; the prefixes required are listed above. It is not a
rendering or parsing problem and must not be worked around.

Result: **65 → 64**, and the reconciliation is that the earlier figure counted the
object table together with something outside it; the object table's own
denominator is 64 of 534.

## Also fixed in the census tool

`ClassifyModel` fed every readable revision to `ReadOdol73StaticModel`. On an
Arma 2 world that would have reported the whole model table as `parse_failed` —
a census defect indistinguishable from a parser one. It now dispatches by
revision the way `ODOLLoader` does. The JSON report also emits the complete
failure list; the text form truncates at twenty, and a truncated backlog cannot
be worked through.

## Evidence

- `PoseidonTools terrain census packages/a2-compat/world/takistan/takistan.wrp --addons inspiration/officialarma`
- Takistan day, dawn and 01:00 captures through the production WGPU renderer at
  free-fly (2600, 700, 450) looking at (2780, 862, 371). Buildings, walls and
  roads present; dawn lights from the east with correct long shadows; 01:00 is
  dark with stars and faint terrain, not a black frame.
- Arma 3 Stratis census unchanged: 350 of 350 static over 163,953 placements.
- OFP Preview-0 WGPU smoke passed.
- `PoseidonTests "[odol]"` with `POSEIDON_ODOL73_FIXTURE` set: 19 passed,
  2 skipped (corpus env var unset).

Licensed reference assets stay local. Nothing under `inspiration/officialarma`
or `packages/` is committed.

## What is not claimed

This is model *parsing* coverage for one world. It says nothing about geometry
correctness, material resolution, or what actually draws. Takistan's terrain
materials, normal maps, vegetation cutouts and road appearance remain open — see
the A2/OA handover backlog. Base Arma 2 ODOL revision 48 remains unsupported and
untested: no revision-48 model is referenced by this world.

---

# Amendment, same day — the corpus gap closed, and 534 of 534

The owner supplied the base Arma 2 licensed data packages
(`ALDP_A2_PBOs_DPL_APL_part1..3`, `ALDP_A2_PBOs_ADPL-SA_APL-SA_part1..3`), taking
the local corpus from 58 archives / 67,417 entries to **111 / 148,218**.

All 64 previously-unresolvable models resolved immediately, and every one of them
is **ODOL revision 48** — precisely the revision this record left unsupported for
want of an asset to test against. No path, prefix or resolver change was needed,
which confirms the CORPUS-INCOMPLETE classification was right.

Revision 48 then needed **no reader work at all**. `ReadOdolA2StaticModel` is
already parameterised by revision and its gates are `< 50`, `>= 51` and `>= 52`;
48 takes the same branches as 49. The only change was to stop refusing it:
`DescribeOdolRevision(48)` becomes `NarrowSubset`, `ODOLLoader` gains a
`loadOdol48Static` mirroring its siblings, and the census dispatches 48 to the
A2-family reader.

```
Takistan, official OA OPRW 24, full A2+OA corpus:
  534 of 534 referenced models parse, 438,906 of 438,906 placements
  revisions: 48 (64 models), 49 (470 models)
  runtime log: 0 "Bad version in LODShape::LoadOptimized", 0 unsupported revisions
```

So the two measured backlogs are closed: **54 → 0** and **65 → 0**.

Gates re-run at this commit: Stratis 350 of 350 over 163,953 placements
unchanged; OFP Preview-0 WGPU smoke passed; `[odol]` 22 passed / 2 skipped and
`[Formats]` 23 passed / 1 skipped; Takistan day capture renders with terrain,
buildings, walls and roads.

The revision-48 claim is still a *static-subset* claim, with the same limits as
49/50/52: geometry parses, nothing here asserts that materials, animation or
proxies are correct.
