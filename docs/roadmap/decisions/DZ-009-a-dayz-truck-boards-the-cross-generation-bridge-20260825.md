# DZ-009 — a DayZ truck boards the cross-generation bridge, data-only

- **Ticket:** follow-on to AST-016B (proxies + bridge addon) and DZ-001 (DayZ is
  an Arma 2 descendant)
- **Date:** 2026-08-25
- **Landed:** data-only — no engine change, no binary deployed. The installed
  exe/DLL pair (`af0ac79f`, per `DEPLOYED-FROM.txt`) ran everything below.
- **Fixture:** DayZ standalone `truck_01_covered.p3d` (ODOL **54**, the V3S
  6x6) from `vehicles_wheeled.pbo` in the owner's DayZ install.

## The pick, and why it is not a tank

The owner asked for a "DayZ tank". DayZ standalone has none — the wheeled
corpus is: civiliansedan, hatchback_02, offroadhatchback (Niva), offroad_02,
sedan_02, transitbus (Ikarus), truck_01 (V3S), truck_02 (M3S-ish), van_01.
No M1025 Humvee either. The most tank-adjacent drivable is **truck_01 — the
V3S 6x6 military truck** (its own damage rvmat still references
`dz\vehicles\wheeled\v3s\data\glass_i.rvmat`, naming it).

Stock parent: **`Ural`**, not `Truck5t` — same 6-wheel military-truck
topology (steered front axle, double rears), East side, and the closest mass
and `wheelCircumference` (4.513) in the stock config. The bridge class is
`CWR_V3S_DZ: Ural` with only `model=` overridden.

## Census (measured)

`truck_01_covered.p3d`: ODOL 54, **13 LODs** — 7 resolution LODs
(LOD 0: 30,669 pts / 28,153 faces / 80 sections / 133 selections / 18 proxies;
down to 114 faces at res 7), shadow volume (res 1100: 18,905 faces), memory
(res 1e15: 175 pts, 126 selections), geometry/fire/view LODs.

- **Selections**: named but **0 vertices** each — the A2-family
  `vertexBoneRefs` gap of AST-016/017, exactly as predicted. Drives fine;
  wheels/doors will not visually animate. Not fought, per the recipe.
- **Memory points are DayZ-English** (`pos_driver`, `wheel_1_1_axis`), not
  OFP's `pos driver`. `TransportType::InitShape` degrades to a computed
  get-in offset when `pos driver` is absent (Transport.cpp:2234) — boarding
  still works, nothing warns, the error latch stays clear.
- **Textures are all `.paa`** — the wheeled-vehicle PBOs ship zero `.edds`,
  so the EddsReader question never arises for this vehicle. DayZ .paa and
  binarised `raP` rvmats read with the existing A2-era loaders.

## The data PBO (`cwr_v3s_dz_data.pbo`, prefix `dz`, 154 MB)

Reference closure measured by string-scanning the kept p3ds + rvmats, not
sampled: 293 files. The covered model + its proxy subtree, crew proxies
(`crew_driver`, `crew_cargo`), and the cross-PBO texture closure pulled from
seven DayZ banks (`dz`, `gear_camping`, `gear_crafting`, `characters_data`,
`vehicles_data`, `vehicles_parts`, `weapons_data` — each mounts under a
`DZ\...` prefix, so one `dz`-prefixed tree reassembles them). Pruned: the
red/rust repaint sets and the command/fire proxy variants (−63 MB), then the
closure re-verified — 5 variant files had to come back
(`red\truck_01_window_cover.rvmat` + 4 rust command-window paas, referenced
by kept proxies). **6 references stay dangling because they are dangling in
DayZ's own shipped data** (`truck_01_wheel_smdi.paa` and 5 green/repair
maps exist nowhere in the corpus); the engine never complained.

No namespace patch was needed — nothing else mounts as `dz`. No config ships
in the data PBO (AST-016B trap 1). Gear-part proxies (`battery_truck`,
`replacement_headlight`, planks) were deliberately left out: the AST-016B
absent-proxy probe skips them at INFO, and the log shows exactly that.

## Verdict (capture, exe+DLL = af0ac79f)

Mission `t72drive.abel`, `CWR_V3S_DZ` added as a fifth `side="EMPTY"`
vehicle. Boot clean: **zero warnings/errors attributable to the vehicle**;
`Objects material coverage: untextured sections=2 (0.169%)` — both stock
Everon scenery, none of the 80 truck sections. Shape loader logs the
canonical ODOL adapter for the truck and every proxy it kept.

Screenshot: the covered V3S renders in-world next to the four tanks. The
visible cargo tarp draws as grey woven canvas with dark panel bands — which
is the **authored** look: the model's tarp binds
`dz\gear\camping\data\shelter_tarp_co.paa` / `tent_pristine_co.paa`, and
both decode to exactly that grey canvas. Textures bind; no fallback white,
no magenta, no blend-transparency surprises.

Honest gaps:

- **Placement cost two extra captures.** The requested spot (5040,3884) is
  74° right of the smoke camera's axis — off-frame; the second spot was
  dead on the sightline occluded by the A1 T-72. Geometry, not rendering.
  Final position 5076.1,3884.6 (≈25 m, 13° left).
- **Not board-tested.** The auto-capture proves spawn + render + clean
  latch; driving is inherited Ural config on populated geometry/land-contact
  LODs and follows AST-016B's boarding result, but no human has driven it
  this session.
- Wheels/doors/steering-wheel will not visually move until the
  `vertexBoneRefs` → named-selection synthesis (AST-016/017 stage 2) lands.
- The grey tarp reads as "untextured" at a glance. It is not — but a
  sceptical owner should look at `shelter_tarp_co.paa` before filing it.

## Installed artefacts (game folder, `@cwr_t72_a1\addons`)

| file | change |
|---|---|
| `cwr_t72_a1.pbo` | rebuilt config: `CWR_V3S_DZ` class + `units[]` entry (742 B, config only) |
| `cwr_v3s_dz_data.pbo` | new, prefix `dz`, 154 MB |
| `dev-missions\t72drive.abel\mission.sqm` | fifth vehicle, `items=5` |

One near-miss recorded so the next session does not repeat it: `config bin`
takes `-o` for output; called positionally it fails, and the follow-on
`pbo pack` then happily ships a **21-byte empty config PBO** over the real
one. Verify the packed size (742 B here) before copying into the game
folder.
