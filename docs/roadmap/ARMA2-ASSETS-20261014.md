# Arma 2 assets that load and work, 2026-10-14

Follow-up to [the Arma 2 mission slice](../verification/arma2-direct-missions-20261012.md).
The owner's goal (2026-10-14): Arma 2 missions are *playable* with Arma 2's own soldiers,
weapons, vehicles, aircraft and objects -- loaded correctly and working, not CWA stand-ins.

## Where it stands (build `364ebb0`)

What a mission class becomes today is decided in `Arma1Compat::ResolveVehicleClass` and
`Arma1ModelClass` (`engine/Poseidon/Game/Compat/`):

| Kind | Model | Behaviour (config, physics, animation) |
|------|-------|-----------------------------------------|
| Objects, buildings, props | Arma 2 model (`A1x_<class>`) | CWA base class picked by simulation |
| Vehicles, helicopters, planes, boats | Arma 2 model | the CWA vehicle of the same kind (`BRDM2_INS` -> `BRDM`, `MV22` -> `UH60`, ...): CWA speed, armour, crew seats, memory-point names |
| Soldiers | **CWA soldier model and moves** (`SoldierWB`/`EB`/`GB`) | CWA. Arma 2 models and moves exist behind `OP_A2_SOLDIERS=1` and **crash on the first frame** (`2e82aea`, Utes bootcamp, 0xC0000005) |
| Weapons, magazines | **CWA weapon** chosen by kind and calibre (`Arma2WeaponFallback`) | CWA |

So vehicles and objects already look like Arma 2, soldiers and weapons do not, and nothing
behaves like Arma 2 beyond what the CWA stand-in does.

## Steps

1. **Measure first.** Per mission: an asset census in the log (every class created, what it
   became, which model, whether the model loaded with LODs and textures) and screenshots of
   the player in first and third person and of the nearest vehicle. Run on the laptop
   (`Downloads\a2test`, see below) over all 98 missions.
2. **Soldiers.** Find the first-frame crash with `OP_A2_SOLDIERS=1` (symbolised stack:
   the CI artifact now carries the PDBs). Then check the Arma 2 skeleton
   (`CfgSkeletons`, `OFP2_ManSkeleton`) against the bones the engine names (head, hands,
   weapon, launcher), moves (BMTR) for stand/crouch/prone/walk/run, entering and leaving
   vehicles, death. When a bootcamp and a campaign mission play cleanly, make it the default.
3. **Weapons.** Generate `A1x_` weapon and magazine classes like the vehicle ones: Arma 2
   model and proxy in the hand, optics (`modelOptics`), muzzle flash, sounds, magazines
   with the Arma 2 ammunition values (`hit`, `typicalSpeed`, `caliber`) on a CWA base of the
   same kind, so firing, reloading and damage work.
4. **Vehicles.** Wheels, tracks, rotors, turrets and doors animate on the Arma 2 model's
   own selections; crew sit in the model's proxies; lights; the vehicle's own values
   (`maxSpeed`, `armor`, `fuelCapacity`, `transportSoldier`) instead of the stand-in's.
5. **Aircraft.** Placed `FLY` starts with engine on and flight speed (the A-10 in
   MPE1_Dogfighters glides down with the engine off), Arma 2 flight values, helicopter
   cargo and doors.
6. **Objects.** Composite map objects placed by their model origin (Utes' LHD is drawn
   in pieces), destruction (`destrType`, ruins).
7. **Play it.** Per mission the start scene with the right people, weapons and vehicles,
   no script errors, and a few minutes played by hand on the laptop.

## Test setup on the laptop

The laptop has no toolchain; builds come from the `Arma 2 / Windows game build`
workflow as a zip (exe, `wgpu_renderer.dll`, PDBs) in `Downloads`.
`Downloads\a2test\go.cmd` (started from Notepad++, F5) reads `job.txt`:

```
DEPLOY=1                # install the newest OpenPoseidon-Windows-*.zip from Downloads
LIST=missions_all.txt   # one mission per line (the 98 of Arma 2 + Operation Arrowhead)
OUT=tests_run11         # one log per mission + exitcodes.txt
SECONDS=40              # --arma2-test-seconds
RENDER=gl33             # WGPU on Vulkan loses the device on this Intel GPU
OP_A2_SOLDIERS=1        # any OP_* / WGPU_* key is passed as an environment variable
```

`stop.cmd` ends a run after the current mission. The deploy writes `DEPLOYED-FROM.txt`
from the zip's `BUILT-FROM.txt` and gives exe and DLL the same time.
