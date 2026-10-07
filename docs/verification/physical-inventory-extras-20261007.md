# Physical Inventory additional features: installed check

Installed source: `b080696c`, branch `codex/private-release-prep-20261006`.
The source commit was pushed to the private safety mirror, built from the
committed tree with `scripts/Build.ps1`, and installed using locked
`scripts/Deploy.ps1`. No original game archive was changed.

## Checks passed

- PoseidonGame build and PoseidonTests build.
- `[combat-assists]`: 17 assertions in 2 cases, including cardinal/wrapped
  bearings and independently calculated vacuum trajectories over flat, uphill
  and downhill surfaces; expired flight and invalid terrain height are rejected.
- Installed native Everon SP fixture: existing inventory open/drop/pickup,
  removed-during-drag and live-capacity conservation regressions still pass.
  All 17 installed original icon hashes match.
- Visual inspection: rifle shows bearing without a grenade range; selected M203
  shows `Impact ~13 m`; Reveal Target captures `Ground mark 19 m` in this scene.
  These are horizontal ground estimates, not an object raycast.
- All three new checkboxes are visible in O → K. Clicking them disables the
  aids immediately and writes zero values in isolated `combat_assists.cfg`.
  Reset restores all three defaults and the HUD appears again. The cached ground
  mark clears when disabling/resetting the aids.
- Locked hosted multiplayer regression: developer harness mutation rejection
  still passes after entering a network world; clean exit and unchanged deployed
  stamp. This does not claim a complete multiplayer match or modified-client
  anti-cheat verification.

SP evidence: `build/physical-inventory/20261007-211053-848`, screenshots 11–16
were inspected in the successful fixture (the same HUD stages were also inspected
in `20261007-210251-037`).
MP evidence: `build/physical-inventory/20261007-210833-865`.
Machine-readable receipts are adjacent to this document.

## Limits and failed fixture diagnosis

The first additional test stopped on `currentWeapon player`, which is not an
available OFP script command. Logs show a test-mode script abort, not a native
crash. The fixture now reads the existing native `ai_combat` selected-slot probe;
it verifies that selecting M203 changes the selected muzzle slot.

The scripted AI-spawn/contact attempt did not elicit a bearing contact report.
Therefore actual audible bearing delivery is **not verified by this smoke**.
The formatter and near-contact branch compile; source review confirms use of
stock `at`, three digit clips, distance buckets, safe subtitle assembly and the
unchanged lockstep radio drain/RNG draw. Distant grid reports and MP take their
existing branches. No custom voice files were installed. The optional fixture
records the sampled radio wave states rather than labelling their absence a pass
for bearing audio.

The displayed ground estimate omits object collisions, bounces, dispersion and
shooter velocity. No performance benchmark beyond the bounded 5 Hz/500-step
implementation is claimed. Broader recoil/medical/fuel changes were reviewed
but deliberately not imported; see
`docs/roadmap/decisions/PHYSICAL-INVENTORY-EXTRAS-20261007.md`.
