# FFV V6 execution

Starting source: 573dd972, asset-obs-001, equal to safety/consolidated-v2.
Starting installed code: ac89cf30. Foreign Cargo.lock preserved.
Scope: OpenPoseidon_FFV_Umsetzungsplan_v6.md, copied from the owner's root checkout.

Assessment: reuse the existing engine paths as proposed. The plan's synthetic
tests are not runtime evidence. Keep its four delivery gates; do not equate
name-neutral weapon eligibility with validated asset coverage.

## Delivery 1, in progress

- Version 3 adds weaponFamily="rifle" as an alternative to the legacy exact
  weapon selector. Unknown/ambiguous families reject the whole permission table.
- One runtime eligibility query checks the selected mode's ammo simulation and
  personal-primary slot semantics; excludes turret, handgun and launcher modes.
  Selection and reload eligibility do not require current aim readiness.
- Use the query in candidate range, initial selection, reload, saved reload
  validation, shot permission and the existing harness reload action.
- Camera ownership/HUD/free-look route uses configured seat capability rather
  than momentary active pose. Actual firing still requires active valid action.
- Final shot insertion checks resolve the actual occupied cargo proxy explicitly;
  the old ProxyWorldTransform fallback is not accepted as a resolved seat.
- Existing integration fixture accepts separate M16/AK74 inventories, never two
  forced primary weapons. Runtime, normal action-menu and content acceptance pending.

Version 0.2.0 content preserves the original authored seat clips. It is not the
generic upper/lower pose adapter or 2x2 vehicle proof yet. Those remain Delivery 2.
Delivery 3 moving/autonomous/lifetime coverage and Delivery 4 families/MP remain open.

Checks so far: 12 existing animation generator tests pass; script parser clean.
Engine build and installed acceptance in progress. No completion claim.
