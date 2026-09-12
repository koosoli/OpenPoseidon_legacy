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

Checks: 12 cargo unit cases / 226 assertions and 12 animation generator tests
pass. Engine build and installed smoke tests run with the default 0.2.0 package.

## Installed evidence

All runs below are under build/jeep-ffv; each keeps its deployment stamp,
package hash, graphics context, responses and captures. Inventories are separate.

- d529d399 / baseline-20260912-132328: M16 NPC shot, reload, save/load and
  scripted exit/reentry passed. AK74 NPC failed to acquire a fire solution.
- 073e1ffa / baseline-20260912-133642: AK74 player fired one real shot and
  damaged himself (0.507391), then died after the second. Confirmed own proxy
  contact, not missing ammunition or an unsupported candidate.
- 3da0fff0 excludes the exact ignored object from nested collision contacts,
  not its vehicle or other occupants. AK74 player hit the target with one round,
  no self damage. Initial keyboard reload tests were not consistently successful.
- 35a0d262 also respects sensor/target exclusions within visibility proxies.
  baseline-20260912-135308: AK74 NPC fire/reload/save/load/lifecycle passed.
  baseline-20260912-135406: AK74 player fire and keyboard reload passed.
  baseline-20260912-135702: AK74 player additionally passed save/load during
  reload, scripted exit/reentry, unsupported seat and destruction checks.
  A held-key probe now records receipt of reload input instead of assuming it.

These tests use scripted boarding and a diagnostic cursor, not normal action-menu
entry or general relative-mouse acceptance. They do not complete Delivery 1.
The first-person capture still shows head/camera misalignment. Head aim presently
compensates the gun transform but not the already-rotated authored head basis;
the next patch shares that neutral basis with target solving and head rotation.
Negative line-of-fire regressions and both rifles must be repeated after it.
