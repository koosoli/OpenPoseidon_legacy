# AST-016C — five helicopters board the cross-generation bridge, one per game

- **Ticket:** follow-on to AST-016B (proxies + bridge addon) and DZ-009 (DayZ
  V3S), same fixture-led track; Milestone 9 stays HOLD
- **Date:** 2026-08-25
- **Landed:** data-only — no engine change, no binary deployed. The installed
  exe/DLL pair (`af0ac79f`, per `DEPLOYED-FROM.txt`) ran every capture below.
- **Owner ask:** one helicopter per game generation, standing as a second row
  behind the tanks in `dev-missions\t72drive.abel`.

## The census (rotor selections are the headline)

| gen | model | ODOL | rotor selections (LOD 0) | verdict |
|---|---|---|---|---|
| A1 | `uh_60.p3d` (UH-60) | 40 | `velka vrtule` **809 faces / 1260 verts**, `mala vrtule` **439 / 665**, blur pair via 2 proxies | **populated — should fully animate**, 2001 code consumes selections directly |
| A2 | `mi24_v.p3d` (Mi-35 corpus) | 48 | `velka/mala vrtule` named but **0 verts** (all 64 selections empty) | frozen rotors — the `vertexBoneRefs` gap of AST-016/017 |
| A3 | `heli_light_01_f.p3d` (MH-9) | 73 | rotors are **6 sub-model proxies** (`main_rotor_f` ×5, `tail_rotor_f`); named selections 0 verts | renders whole via resolved proxies, rotors frozen |
| DZ | `wreck_uh1y.p3d` | 54 | **0 selections** in the file | static wreck by construction |
| RF | `UH1H_base.xob` → MLOD p3d | — | **no rotor meshes in the base xob** (Reforger keeps rotors as separate prefabs); no selections survive conversion | static |

DayZ standalone ships **no flyable helicopter model** — its aircraft corpus is
wrecks only (`wreck_mi8`, `wreck_uh1y`, `wreck_c130j`, `wreck_santassleigh`).
The UH-1Y wreck is the pick; it boards as scenery, honestly.

## Classes (all in the rebuilt `cwr_t72_a1.pbo` config, 1238 B binarised with `-o`)

| class | parent | model | note |
|---|---|---|---|
| `CWR_UH60_A1` | `UH60` | `\ca\air\uh_60` | stock parent exists, flies on populated selections |
| `CWR_MI35_A2` | `Mi24` | `\ca\air2\mi35\mi24_v` | stock Mi24 exists |
| `CWR_MH9_A3` | `UH60` | `\a3\air_f\heli_light_01\heli_light_01_f` | **stock CWA has no OH58/Kiowa vehicle class** (checked the debinned config: Cobra, Ch47D, UH60, Mi24, Mi17 only) — UH60 is the substitute parent |
| `CWR_UH1Y_DZ` | `NonStrategic` | `\dz\structures\wrecks\aircraft\wreck_uh1y` | static |
| `CWR_UH1H_RF` | `NonStrategic` | `\reforger\uh1h\assets\vehicles\helicopters\uh1h\uh1h_base` | own mod `@cwr_uh1h_rf` (config 394 B + data pbo), per AST-016B |

## Nested bank prefixes, and the theft rule that shaped them

`QIFStreamB::AutoBank` picks the **longest** matching prefix (comment cites
`a3\map_stratis\data` vs `...\layers`), so nested prefixes are safe — with one
sharp edge: a nested bank **steals every deeper path** from the shallower bank,
and a stolen lookup *fails* rather than falling through. Consequences here:

- `ca\air`, `ca\air2`, `ca\data` are new banks beside the existing `ca` (which
  holds only `tracked\` + `weapons\`): nothing stolen.
- A `ca\weapons` bank would have stolen the T-72's dshk/zasleh files —
  **not created**; the one file the Mi-35 needs
  (`ca\weapons\data\detailmaps\metal_rough_dt.paa`) went into a repack of
  `cwr_t72_a1_data.pbo` instead (43 → 44 files).
- `dz\structures` beside `dz`: the V3S bank has no `structures\` paths, safe.
  `dz\data\data\default.rvmat` (referenced by the wreck, absent everywhere)
  went into a repack of `cwr_v3s_dz_data.pbo` (293 → 294 files) rather than a
  `dz\data` bank, which would have stolen the V3S's 54 `data\` files.
- `a3\air_f` + `a3\data_f` beside `a3` (armor-only): safe.

A2 corpus prefix collision, resolved by merger not patching: A2's `ca\air`
*shared* files (`rus_heli_cross`, `flare_lights_front`, …) live in the same
`ca\air` bank as the A1 UH-60; the three same-name files were byte-compared —
2 identical, `bis_klan.paa` differs (clan-logo placeholder, A1's wins,
cosmetic). No binary namespace patch was needed anywhere.

Closures: A2 wreck rvmats reference `launcher_nohq/smdi.paa` under
`aircraft\data` — **dangling in DayZ's own data** (the files exist only under
`vehicles\data` in `structures_wrecks.pbo`); both were copied across so the
references resolve here. The Reforger conversion in the inherited state was
half-finished (4 textures); re-run against the addons `data` **directory**
(AST-016B rule): 32/37 parts textured, 17 paa + 9 normals baked, geometry LOD
with 8 components.

## Installed artefacts

`@cwr_t72_a1\addons`: rebuilt `cwr_t72_a1.pbo` (config), repacked
`cwr_t72_a1_data.pbo` (+detailmap) and `cwr_v3s_dz_data.pbo` (+default.rvmat),
new `cwr_heli_a1_data.pbo` (prefix `ca\air`, 13 MB), `cwr_heli_a2_data.pbo`
(`ca\air2`, 72 MB), `cwr_heli_ca_data.pbo` (`ca\data`, 1.7 MB),
`cwr_heli_a3_data.pbo` (`a3\air_f`, 69 MB, foreign config.bin stripped),
`cwr_heli_a3_shared_data.pbo` (`a3\data_f`, 826 KB), `cwr_heli_dz_data.pbo`
(`dz\structures`, 32 MB). New mod `@cwr_uh1h_rf\addons`: `cwr_uh1h_rf.pbo` +
`cwr_uh1h_rf_data.pbo` (prefix `reforger\uh1h`, 62 MB). Mission: five
`side="EMPTY"` vehicles added, `items=10`, `addOns[]` gains `CWR_UH1H_RF`.
Two mods load as one semicolon-joined `--mod` value (the option is a single
semicolon-separated string; repeating the flag is not the mechanism).

## Verdict (three captures, exe+DLL = af0ac79f)

Boot clean every run: **zero `[ERROR]` lines, no error latch**, all five
foreign models logged through their adapters (canonical ODOL ×4, MLOD ×1).
Untextured sections 17 of 3088 (0.55%) — stock scenery plus the UH-1H's two
decal sections (its `Decals` emats name no BCRMap; known conversion gap).

**The prescribed placement was behind the camera.** The requested row
(z 3908–3918) sits *north* of the smoke player (z 3902.55, azimut 151 ≈ SSE,
looking toward decreasing z) — the same axis geometry DZ-009 paid two captures
to learn. Capture 2 (row moved beyond the tanks at ~45–55 m) was still empty
of helicopters: the two near tanks at 10–17 m subtend ±20° each and occlude
almost the whole frustum. Capture 3 placed the row inside the measured free
wedges (bearings 121–147 and 183–189, plus the above-silhouette band): the
**MH-9 is visibly in frame** (red/white fuselage right of the V3S tarp);
UH-60, Mi-35, UH-1Y and UH-1H are spawn-confirmed by the shape-loader log but
read as slivers/roofline pixels at 45–85 m in tall grass from this fixed
first-person camera. The final positions (x 5054–5109) deviate from the
requested x 5050–5080 band for exactly this reason, recorded here.

Honest gaps:

- Only the MH-9 is *unambiguously* identifiable in the verification PNG; the
  other four are log-verified (loaded + placed, zero errors) but not
  eyeballed. A human walking the mission is the real check.
- A1 UH-60 rotor animation is predicted (populated selections + stock UH60
  config), not observed spinning — the wreck-camera never lifts off.
- A2/A3 rotors frozen until AST-016/017 stage 2 (`vertexBoneRefs` synthesis).
- UH-1H albedo reads dark (Reforger BCR-alpha-is-roughness, chip already
  filed per AST-016B) and its two decal sections draw untextured.
- Three captures against a budget of two; the third was placement geometry,
  not a rendering failure, and stopped there.
