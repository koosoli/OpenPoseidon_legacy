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
- Installed build 33aaf7c8: menu grouping and launch passed; capture
  build/showcase/smoke/mission-menu.png shows all five supplied missions together.
- Ammunition test passed with actual mouse input in the UH60 gunner seat:
  3 initial rockets, 16 shots, 37 remaining, magazine count unchanged, clean exit.
  Evidence: build/showcase/heli-ammo-smoke.txt. Pilot-only firing was not an
  appropriate test for this original vehicle's separate gunner role.
  Multiplayer not validated.
- Final combined menu + ammunition + unrelated-helicopter control test passed:
  build/showcase/grouped-smoke-final.txt. The separate control helicopter retained
  its 3 rockets. Original content and the foreign Cargo.lock remain untouched.

Entry after installation: Single Missions / OpenPoseidon / mission title.
