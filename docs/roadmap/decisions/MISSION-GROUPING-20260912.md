# Default mission grouping and sandbox ammunition

Baseline: 430f2f0c, asset-obs-001.

Request: group our supplied missions under OpenPoseidon and give the showcase
helicopter unlimited ammunition. No changes to original missions or global ammo.

- Existing GetSPMissionLookupDirs / DisplaySingleMission::LoadDirectory already
  merge same-named folders from active mods. No menu implementation added.
- Installers and VehicleActions packaging now target Missions/OpenPoseidon.
  Migration preserves mission contents, refuses differing destination files and
  converging source conflicts before writes, and rejects linked install paths.
- Showcase labHeli uses a Fired handler with setVehicleAmmo 1. This command updates
  existing local magazines in place, clamps its fraction, rejects nonfinite input,
  and does not allocate reserve magazines or change unrelated entities.
  Fired runs before ammunition subtraction: the resulting magazine can be one
  shot below full, but is replenished again on every shot.
- Build passed. Migration fixtures passed including idempotence, stock/user
  preservation, legacy and installed mission grouping, conflicts and junctions.
- Installed menu/ammunition smoke test pending. Multiplayer not validated.

Entry after installation: Single Missions / OpenPoseidon / mission title.
