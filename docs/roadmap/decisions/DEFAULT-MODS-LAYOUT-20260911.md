# Canonical default mod content directory

Owner request: place our gameplay content under the game's Mods directory,
not beside the executable or mixed into original AddOns/Missions.

Prepared, NOT installed or runtime accepted yet. Migration fixtures passed
20260911-201913: idempotence, user file preservation, conflicts and junctions.
The combined snow/default-content suite passed 9998 assertions in 15 cases.

Canonical layout:

```
Mods/@OP_VisualUpgrade/AddOns/op_ground_materials.pbo
Mods/@OP_VehicleActions/AddOns/op_jeep_actions.pbo
Mods/@OP_VehicleActions/Missions/jeep-manual.Intro/
Mods/@OP_VehicleActions/Missions/jeep-ride.Intro/
Mods/@OP_Fusion/AddOns/fusion_ofp.pbo
Mods/@OP_Fusion/Missions/FusionTour.FusionOFP/
Mods/@OP_Showcase/AddOns/showcase_lab.pbo
Mods/@OP_Showcase/Missions/ShowcaseLab.Intro/
Mods/@OP_SizeProbe/AddOns/fusion_size_probe.pbo
Mods/@OP_SizeProbe/Missions/SizeProbe.FusionSizeProbe/
```

Each package carries mod.json and uses the existing automatic content resolver.
Original -> default content -> explicit user mods priority stays unchanged.
The existing additive mission discovery already scans active Mods/Missions;
no parallel mission registry or new VFS is required. Missing optional worlds
are not generated/distributed by an ordinary engine build. Existing installed
worlds are relocated without regenerating their original-derived content.

Move-DefaultContentToMods.ps1 preflights paths, reparse points and conflicting
files before moving named owned content. Identical duplicates may be removed;
different files abort instead of silently overwriting them. No recursive
deletion, no stock cwr_logo.pbo, Resistance missions or third-party mods touched.
Executable, DLLs, runtime configuration, saves, caches and diagnostics are not
gameplay asset packages and are not swept into Mods indiscriminately.

Pending acceptance: installed mission discovery; Fusion/Showcase loading;
missing/disabled/user-priority tests; root residue audit and clean shutdown.
Jeep firing acceptance is parked by the owner, not required to relocate its files.
Old instructions naming Missions/FusionTour or Missions/ShowcaseLab refer to
the pre-migration installation, not the new physical paths.
