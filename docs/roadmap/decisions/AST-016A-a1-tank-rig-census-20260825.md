# AST-016A — the Arma 1 T-72 already speaks OFP; the rig is populated and the selection names match the engine's hardcoded ones

- **Ticket:** AST-016A (fixture-led, under AST-016/017; does **not** un-hold the
  Milestone 9 asset-compatibility programme)
- **Date:** 2026-08-25
- **Status:** measurement only. **Nothing was built, deployed or captured.** Every
  number below comes from `PoseidonTools` run offline against files on this
  workstation, and from reading the engine source. Installed build at the time of
  writing: `feat/consolidated 5749545f 2026-08-24_23:22` (`DEPLOYED-FROM.txt`),
  the same commit as the working-tree HEAD — no stale-binary risk, and no binary
  was involved in these numbers anyway.

## Why this was measured

[AST-016-017-animation-gap-scoping-20260814.md](AST-016-017-animation-gap-scoping-20260814.md)
established that ODOL revision 40 (Arma 1) still carries skinning in **named
selections**, the way OFP does, while revision 48+ (Arma 2 onward) moved
membership into `vertexBoneRefs` and leaves the selections present but **empty**.

That measurement was taken on a **character** (`civil_1_tshirt.p3d`,
`ofp2_manskeleton`). The same document lists as explicitly unmeasured (`:590`):
Arma 1 named-selection *weight* population, and whether Arma 1 models also carry
`vertexBoneRefs`. No rev-40 **vehicle** census existed anywhere in the tree.

The open question this record closes: **can an Arma 1 tank drive the existing
2001 animation machinery with no synthesis layer?**

## Answer

Yes. The A1 T-72 is not merely parseable — its selection and memory-point names
are the *same names the engine hardcodes*, because Arma 1 inherited the OFP model
conventions wholesale.

## Fixture

| what | path |
|---|---|
| Binarised (ODOL 40) | `packages/a1-compat/addons/tracked.pbo` -> `t72.p3d`, 3,115,602 bytes, dated 2007-05-16 |
| **Unbinarised (MLOD P3DM v257)** | `inspiration/officialarma/A1SM_Data_DPL_APL/ASM_Data_APL/tracked/T72.p3d`, 13,740,459 bytes |
| **Plaintext config** | `inspiration/officialarma/A1SM_Data_DPL_APL/ASM_Data_APL/tracked/config.cpp`, 30,388 bytes — `class T72: Tank` at `:781` |
| Textures / rvmats / RTMs | 38 files extracted to `.tmp-a1-tank/tracked/` |

Both model copies are the same vehicle; the Arma 1 **Sample Models** release ships
the editable MLOD *and* the plaintext config. This was not known when the work was
planned, and it removes two assumed obstacles at once (see "What this changes").

## Measured — ODOL 40 `t72.p3d`

19 LODs. Resolution LODs 0-6 (15,165 -> 355 points), shadow 1000/1200, and the
full special-LOD set: Geometry `1e13`, Memory `1e15`, LandContact `2e15`,
HitPoints `5e15`, ViewGeometry `6e15`, FireGeometry `7e15`.

### Resolution LOD 0 — selections are populated

64 selections, 17 sections, 6 proxies. Bone-named selections carry vertices:

| selection | verts | role |
|---|---|---|
| `telo` | 14,603 | hull body |
| `otocvelitele` | 4,314 | commander turret |
| `otocvez` | 3,039 | **main turret** |
| `otochlavenvelitele` | 2,407 | commander gun |
| `damagehide` | 1,162 | damage |
| `koll1` / `kolp1` | 418 each | drive sprockets |
| `koll2` / `kolp2` | 548 each | idlers |
| `kolol1..6`, `kolop1..6` | 135 each | 12 road wheels |
| `otochlaven` | 253 | **main gun** |
| `pasoffsetl` / `pasoffsetp` | 252 each | **track offset** |
| `poklop_driver` / `_gunner` / `_commander` | 146 / 291 / 147 | hatches |
| `podkolol1..6`, `podkolop1..6` (+ `_hide` twins) | 14-26 | track deform |

Contrast, from the AST-016/017 doc: A2 `BlackHead.p3d` (rev 48) — 85 selections,
**85 empty**; A3 `basicbody.p3d` (rev 73) — 90 selections, **90 with 0 vertices**.

### Memory LOD (`1e15`) — every axis and point the engine asks for

36 selections, all populated: `osaveze`, `osahlavne`, `usti hlavne`,
**`konec hlavne`**, `otocvez`, `otochlaven`, `osavelitele`, `osahlavnevelitele`,
`otocvelitele`, `otochlavenvelitele`, `kulas`, `gun_muzzle`, `gun_chamber`,
`osa_poklop_driver` / `_gunner` / `_commander`, `driverview` / `gunnerview` /
`commanderview`, `zamerny`, `vyfuk start` / `vyfuk konec`, `stopa ll/lr/rl/rr`,
`l svetlo` / `p svetlo` (+ `konec l/p svetla`), `pos driver`, `pos driver dir`,
`doplnovani`, `commander_weapon_view`, `machinegun`.

`konec hlavne` was flagged VERIFY in the plan (only `usti hlavne` had been
confirmed, on the A2 model). It exists here.

### Geometry LOD (`1e13`) — named components present

14 selections: `component01`…`component09`, plus `otocvez`, `otochlaven`,
`otochlavenvelitele`, `damagehide`, `damagevez`. This is the OFP geometry
convention.

**Mass: the code path is sound; the value for this fixture is still unmeasured,
because no tool prints it.** Reading the source rather than the file: COL-001
already landed mass for this revision. `Odol40.hpp:168-172` reads the per-point
`massArray`, `mass` and `invMass` deliberately ("a model that arrives with mass 0
is `Object::IsPassable()` — a soldier walks through it"), `ODOLLoader.hpp:239-244`
forwards all of it plus `centerOfMass` into the IR, and `:256-264` **recomputes**
the total from the array when the stored `mass` is not positive. `Model.hpp:351-357`
holds the fields. So the plan's feared silent-zero-mass failure is already
defended in code for rev 40.

What is missing is observability: no `PoseidonTools` subcommand surfaces P3D mass
(`ModelCommand.cpp` has no `mass` reference; only `XobCommand.cpp` mentions one).
That tooling gap is why the number was never taken, and it is the cheapest thing
to add before Stage 3.

### Proxies — stated correctly in the file, still dropped at conversion

6 in LOD 0, each with a named-selection and bone index:

| proxy | namedSel | bone | section |
|---|---|---|---|
| `\ca\temp\proxies\t72\driver` | 22 | 56 | 0 |
| `\ca\temp\proxies\t72\commander` | 24 | 75 | 0 |
| `\ca\temp\proxies\t72\gunner` | 25 | 80 | 0 |
| `\ca\wheeled\flag_alone` | 48 | 80 | 0 |
| `\ca\weapons\zasleh2_proxy` (seq 1) | 62 | 76 | 2 |
| `\ca\weapons\zasleh2_proxy` (seq 2) | 63 | 74 | 1 |

They are still dropped at conversion — `convertOdol73Mesh`
(`ODOLLoader.hpp:329-548`), which every revision 40+ passes through, never touches
`mesh.proxies`; the only writer is `convertProxies` (`:782`), reached solely from
the ODOL-7 path. Unchanged from the AST-016/017 finding, now confirmed on a
vehicle fixture.

## The engine's side of the contract

The decisive result is not that the model parses — it is **name-level agreement**
with code written in 2001. From `TankTurret.cpp`:

| engine, hardcoded | site | present in A1 T-72 |
|---|---|---|
| `"PasOffsetL"` / `"PasOffsetP"` | `TankTurret.cpp:90-91` | `pasoffsetl`, `pasoffsetp` — 252 verts each |
| `"zasleh"` | `TankTurret.cpp:96` | `zasleh`, `zasleh_1` |
| `"kulas"` | `TankTurret.cpp:108` | memory point, 1 vert |
| `"usti hlavne"` / `"konec hlavne"` | `TankTurret.cpp:104-105` | memory points |
| `FindNamedSel("otocVez")` | `TankOrCar.cpp:60` | `otocvez`, 3,039 verts |

And from config, read but not hardcoded — `TurretType::InitShape`
(`TankTurret.cpp:252-256`) takes `turretAxis`, `gunAxis`, `gunBeg`, `gunEnd`;
`TurretType::Load` (`:242`) takes `minElev` / `maxElev` / `minTurn` / `maxTurn` /
`soundServo`; `TankType::Load` (`:51-52`) reads them from subclasses **`Turret`**
and **`ComTurret`**; `TankType::InitShape` (`:140-145`) reads `Wheels` ->
`rotL[]`, `rotR[]`, `upDownL[]`, `upDownR[]`, where `upDown*` is a flat
(wheel, track) pair list.

The T-72 supplies both turrets' worth of memory points, so `ComTurret` maps onto
the commander's DSHKM rather than being stubbed, and `podkoloX` /
`podkoloX_hide` are exactly the wheel/track pairs `upDown*` wants.

## The config gap, and why it is smaller than assumed

`PoseidonTools config debin` **fails** on the A1 `config.bin` (33,624 bytes, from
`packages/a1-compat/addons/tracked.pbo`) — measured, same as the A2 one. The
header is well-formed standard Arma rapified
(`00 72 61 50 | 00000000 | 08000000 | …`, then `CfgMovesBasic`,
`CfgMovesMaleSdr`, `CfgPatches`), so this is a **reader limitation, not a corrupt
file**. Worth a ticket of its own; not on this path's critical line.

It is not on the critical line because the **plaintext** `config.cpp` ships in the
Sample Models package. Note it is the *Arma 1* schema — `class Turrets { class
MainTurret { … class Turrets { class CommanderOptics … } } }` — not OFP's flat
`Turret` / `ComTurret`. The bridge is a hand-written translation either way; the
sample config is the value source (armor 690, maxSpeed 60, minElev -6 /
maxElev +14, crew, sounds, `class Exhausts` using `vyfuk start` / `vyfuk konec`,
`hiddenSelections[] = {"pruh"}`), not a drop-in.

## What this changes about the plan

1. **No synthesis layer is needed for the A1 fixture.** The plan's Stage 2
   `vertexBoneRefs` -> named-selection synthesis is an **Arma 2+ concern only**.
   A1 reaches a turning turret through the untouched 2001 path.
2. **The MLOD source may remove the proxy work too.** `MLODLoader` already
   synthesises proxies from selections (`MLODLoader.hpp:109-114`, `:163-168`).
   Loading the Sample-Models MLOD sidesteps the rev-40 proxy drop entirely for
   *this fixture*. The ODOL-40+ proxy conversion remains the right general
   capability and is still wanted — it just stops being a blocker for a drivable
   A1 tank.
3. **Ordering is confirmed.** A1-first was correct, by a wider margin than the
   evidence available at planning time suggested.

## Still open / not measured

- **Geometry-LOD mass — the fixture's value.** The read/forward/recompute path is
  confirmed correct in source (see above), so this is downgraded from a risk to a
  measurement. Add a mass line to `model inspect` and take the number before
  Stage 3, rather than discovering a zero through gameplay.
- **`vertexWeights` population** on the A1 selections — still unmeasured, as in
  the parent doc. For a tank the parts are rigid, so dominant-influence behaviour
  is expected to be indistinguishable; this matters for A1 *characters*, not here.
- Whether A1 models *also* carry `vertexBoneRefs` (parent doc `:591`).
- Whether `ShapeAdapter` tags the special LODs with the resolution constants the
  OFP lookups (`MemoryLevel()`, `FindMemoryLevel()`, geometry) expect.
- The A1 `config.bin` debinarizer failure — separate ticket.
- Second data point: `M1_abrams.p3d` has **not** been censused. Do not generalise
  to "Arma 1 vehicles" from one vehicle; this record claims **one fixture**.

## Reproducing

```
PoseidonTools pbo extract packages/a1-compat/addons/tracked.pbo .tmp-a1-tank/tracked -f t72
PoseidonTools model inspect    .tmp-a1-tank/tracked/t72.p3d
PoseidonTools model selections .tmp-a1-tank/tracked/t72.p3d
```

Full selection dump retained at `.tmp-a1-tank/t72-selections.txt` (1,004 lines).
