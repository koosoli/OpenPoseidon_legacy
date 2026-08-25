# AST-016B — proxies for every ODOL revision, and one bridge addon that boards three generations of tank

- **Ticket:** AST-016B (fixture-led, under AST-016/017; Milestone 9 stays HOLD)
- **Date:** 2026-08-25
- **Landed:** `aa4c4c75` (hidden-proxy flag, MAT-046), `2f8898bc` (proxy
  conversion + boardable A1 T-72), plus this session's A3 extension (data-only,
  no engine change needed).
- **Fixtures:** A1 `t72.p3d` (ODOL 40), A2 `t72.p3d` (ODOL 48, census only),
  A3 `mbt_02_cannon_f.p3d` (ODOL 73, T-100 Varsuk, from the owner's installed
  Arma 3 `armor_f_gamma.pbo`).

## Engine changes (all in `2f8898bc`)

1. **Proxy conversion for revisions 40–73** — `convertOdol73Mesh` now forwards
   the LOD header's proxy records into the IR, mirroring the v7
   `convertProxies`. BIS rows are `[aside, up, dir, pos]` on both paths.
   Measured: A1 T-72 reports 6 proxies on LOD 0; A2 T-72 reports the same 6
   where the selection-synthesis fallback produced **zero** (A2 proxy
   selections are empty like all its selections); A3 T-100 reports its
   crew/muzzle/light proxies with A3 paths.
2. **Absent proxy models degrade to absent proxies** — `ShapeAdapter` probes
   `QIFStreamB::FileExist` before `NewProxyObject`. A1/A3 crew placeholders
   (`ca\temp\proxies\…`, `a3\data_f\proxies\…`) are not files in any mounted
   bank; letting `Shapes.New` discover that raised `WarningMessage`, and **one
   warning fails the whole mission** through `InitVehicles`' error latch
   (`GetMaxError() >= EMError`). Skipped at INFO now. OFP data unaffected —
   its proxy shapes all exist, the probe never fires.
3. **The silent boot failure is silent no more** — `StartAutoTest` logs
   `GetMaxErrorMessage()` when the latch killed the boot. The message was
   stored for a dialog and never logged; a headless mission failure had no
   reason on record. Finding this cost four blind runs; with it, the next two
   failures named themselves instantly ("missing addons: cwr_t72_a1", then
   "Cannot open object ca\temp\proxies\t72\driver.p3d").
4. **`pbo pack --prefix`** — writes the retail `prefix` header property;
   `FileBankManager::Create` took a property table all along and nothing
   passed one. `--test-model` also prints per-LOD **resolved** proxy counts
   (what the engine can draw, not what the file declares).

## The bridge addon (`@cwr_t72_a1`, game folder)

| PBO | contents | prefix |
|---|---|---|
| `cwr_t72_a1.pbo` | binarised config only | — |
| `cwr_t72_a1_data.pbo` | A1 t72.p3d + textures/rvmats + zasleh proxies | `ca` |
| `cwr_t100_a3_data.pbo` | A3 mbt_02 subtree + shared `armor_f\data` palettes, **config.bin removed** | `a3` |

Config: CfgPatches `CWR_T72_A1` (units `CWR_T72_A1`, `CWR_T100_A3`), stock
classes forward-declared with empty bodies (the stock BIS idiom), two new
classes inheriting stock `T72` with only `model=` (+ cosmetic armor/maxSpeed on
the T-100). **No stock class is redefined; without `-mod` the game is
byte-identical stock.** Verified: stock boot with no mod, zero errors.

Mission `dev-missions\t72drive.abel`: A1 T-72, stock T72 and A3 T-100 side by
side, `addOns[]`/`addOnsAuto[]` naming the CfgPatches class (mandatory — the
engine refuses the mission otherwise, and only the new latch log line says so).

## What works per generation, and why (matches AST-016A's prediction)

| | A1 (rev 40) | A3 (rev 73) |
|---|---|---|
| renders, boards, drives, aims, fires | yes | yes |
| turret/gun/wheels **visually** move | **yes** — selections populated, 2001 code consumes them directly | **no** — same selection *names* (`otocvez`!), 0 vertices; membership lives in `vertexBoneRefs` |
| memory points (`osaveze`, `usti/konec hlavne`, `driverview`) | populated | **populated** — BI kept the Czech memory-point convention through Arma 3, so `TurretType::InitShape` initialises correctly |

The A3 (and A2) visual-animation gap is exactly the `vertexBoneRefs` →
named-selection synthesis of AST-016/017 stage 2 — one seam in
`ShapeAdapter`, feeding `Skeleton::Prepare`, `TurretType` and `Animation.cpp`
alike. It is the single remaining piece between "drives with a frozen turret"
and "fully animated" for every post-A1 vehicle.

## Traps recorded for the next bridge

- **Never `--addon-root` a foreign game's addon directory for gameplay.** Its
  configs parse into the addon registry and break stock classes (measured:
  mission unbootable with the A1 addons dir mounted). Ship a data PBO with no
  config instead.
- The mission-smoke harness stages **only** `mission.sqm` + `description.ext`
  — an `init.sqs` never reaches the game. Put vehicles in `class Vehicles`
  with `side="EMPTY"`.
- The foreign PBO's own `config.bin` must be stripped from data PBOs even when
  it sits below the bank root (defence in depth; the root-only parse rule is
  an implementation detail).

## Addendum — Reforger BTR-70 (same day)

The scout's "hours" estimate held: `xob convert` on
`Assets/Vehicles/Wheeled/BTR70/BTR70_body.xob` -> MLOD p3d -> `pbo pack
--prefix 'reforger\btr70'` -> `class CWR_BTR70_RF: Truck5t` (turretless wheeled
parent, since the body-only conversion has no selections or memory points) ->
boots, boards, drives in `btrtest.abel`. Mod folder `@cwr_btr70_rf`, separate
from the ODOL bridge mod.

Two measured findings for the next conversion:

1. **Pass the addons `data` DIRECTORY to `xob convert`, never a single pak.**
   Vehicle materials reference shared detail tiles in OTHER paks
   (`Assets/_SharedData/Metal/ST_*_BCR.edds` etc.); against one pak, 10 of 17
   materials fail with "BCRMap .edds does not decode" (a not-found conflated
   with a decode failure) and the vehicle renders white. Against the
   directory: 12/17 resolve, 26 paa baked, 41/47 parts textured.
2. **Baked vehicle albedo reads near-black in the classic renderer.** The
   converter's own note names the suspect: Reforger's BCR alpha channel is
   ROUGHNESS, folded into the baked `_ca` alpha -- which the classic pipeline
   interprets as transparency/cutout. PBR->classic albedo conversion for
   vehicles is an open tuning task (world models did not hit it this hard;
   their materials are MatPBRBasic with sane alpha).

## Addendum 2 — why the Reforger vehicles were near-black (2026-08-26)

The suspect named above (BCR roughness alpha read as opacity) was measured and
ACQUITTED. The mechanism was untextured overlay shells drawn opaque on top of
the body.

Measured, in order:

1. **Alpha is clean.** Every baked body tile classifies OPAQUE (alpha min=max=255);
   the only `_ca` cutouts are glass, grilles and one resolved decal — all
   legitimately alpha-tested. The `Write` path already forces alpha to 255 when
   the material names no OpacityMap.
2. **The albedo values are in range.** The tinted base tile bakes to mean sRGB
   (63, 61, 46) — statistically the same as stock CWA olive armour
   (`bmp2_vez_back.pac` mean (64, 63, 44), `bmp2_top.pac` (91, 91, 68)). No
   linear->sRGB transfer is missing; `LinearToSrgb` on the `Color_N` targets
   already lands where 2001 artists painted.
3. **The black was a decal shell.** The capture log names it:
   `BTR_70_base_Decals_01.emat`, section 1, **20,139 indices** (6,713 triangles
   wrapping the whole hull), "draws UNTEXTURED ... no face texture and the RVMAT
   bound no albedo" — plus `interior_Decals_01` (8,847) and `Decals_03`.
   `MatPBRDecal` is Enfusion's alpha-blended overlay family; its albedo lives in
   keys the baker does not resolve, so the shell drew opaque fallback grey over
   the olive body. Measured hull pixels: (44, 48, 46) — neutral GREY, not
   shaded olive, which is what said "different material on top".

Fix (XobCommand.cpp): a material that resolves NO albedo and is `MatPBRDecal`
family — or a `*WaterErase*` utility surface, Enfusion's invisible water
suppressor — has its parts dropped whole (`BakedMaterial::hideFaces`). A decal
whose albedo DOES resolve (BTR `Decals_02`) still bakes and alpha-tests.
Texture output is byte-identical before/after (verified `cmp` on shared tiles
and the global mask), so deployed worlds are untouched.

After: BTR-70 212,003 tri (9,772 dropped), UH-1H 148,410 tri (2 decal
materials dropped). Capture `btrtest.abel`: sunlit plates mean (84, 84, 76) —
olive-green; the shade side keeps the olive hue (48, 52, 43, B < G) instead of
neutral grey. Not black, not white. Shade-side brightness is the classic
renderer's ambient term, a separate (and stock-consistent) matter.
