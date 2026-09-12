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
474d8993 shares the animated neutral head basis with target solving and head
rotation. The first-person capture now shows the personal rifle; the Jeep roof
still occupies the upper view. This is not a general pose-quality acceptance.

## Normal input follow-up

- 474d8993 / 140230: AK74 player reload/save/lifecycle passed with the diagnostic
  cursor. 140331 AK74 friendly blocker and 140404 M16 blocked-shot cases passed.
- ccdaa746 includes swept-query thickness in the collision broadphase sphere.
  Combined seat/collision tests: 13 cases, 232 assertions pass; animation generator:
  12 tests pass. Carrier and other occupants are not globally ignored.
- Menu fixture uses the assigned passenger's real entry point and Return, not a
  fake moveInCargo action. A nonleader without assignment correctly has no entry
  action; selecting the last action instead of the default had selected inventory
  actions. These fixture mistakes were corrected without changing OFP group rules.
- 5f643497 / 145224 proved that normal boarding requeued the completed
  combatgetincar external action. Cursor was inside the arc, freelook false,
  capability valid, but the stale action blocked the seated action graph.
- 9280c2b1 discards only the completed external action before the FFV seat switch;
  queued future actions remain. 145548 AK74 and 145708 M16 passed normal menu
  reentry, relative mouse entering/leaving/returning to the firing arc and actual
  mouse-button shots. No crew_cursor in this branch. Initial exit was scripted.
- 150406 AK74 additionally passed keyboard reload and firing afterwards. M16
  150457 failed to begin reload despite held input. Do not count earlier sporadic
  reload successes as proof that the input route was reliable.
- 3becc1a3 moves player reload input from carrier BasicSimulation to the existing
  per-frame UI path. Existing action/context and magazine logic remain unchanged.
  The test now uses the normal menu for exit as well as reentry, and waits for both
  the reload action and the weapon's existing load timer. 151132 M16 and 151226
  AK74 passed normal menu exit/reentry, relative mouse arc transitions, shots,
  keyboard reload and subsequent shots. 151319 M16 NPC fire, reload, save/load
  during reload and lifecycle passed. 151513 AK74 player save/reload/lifecycle,
  151615 AK74 friendly blocker and 151647 M16 blocked-shot regressions passed.

Delivery 1 functional input checks pass for the two separate rifle loadouts.
This does not establish unrestricted first-person pose quality or all mission
actions. Deliveries 2-4 remain open, including the
second vehicle/seat pose adapter, holdout profile, independent occupants,
autonomous moving AI, broader weapon families and multiplayer. MP stays disabled.

## Delivery 2 baseline inspection

Original installed CONFIG.BIN resolves Truck5tOpen to model truck5t2, inherited
12 cargo places; its first two are truck co-driver actions, subsequent places
use ManActCargo. UAZ has a distinct UazCoDriver plus Cargo action. No profile
was copied onto either vehicle and no original archive was changed.

3becc1a3 / 152232: Truck5tOpen cargo index 2 resolves cargover4, seat forward
approximately [0.995, 0.070] in world X/Z instead of the Jeep's [0, 1].
152254: original UAZ cargo 1 recorded separately. Inspection fills earlier
places with real occupants because this engine's moveInCargo accepts an object,
not an invented [vehicle,index] command overload. These are original baselines,
not proof of FFV on the new vehicles. The truck's open bed is the second-vehicle
candidate; actual lower-body seat preservation and neutral upper-body frame
adaptation remain necessary before shipping that profile.

## Player action-menu activation (owner request)

The owner requested that the shooting pose be explicitly activated through the
action menu rather than automatically on boarding. This supersedes the automatic
player stance used in the earlier Delivery 1 measurements, not the common rifle
or final shot-permission paths.

- Structural availability is now GetAvailableCargoWeaponSeat. The configured
  player route also requires a saved cargoWeaponRequested flag, default false.
- Ready personal weapon / Waffe im Fahrzeug bereitmachen enables the existing
  stance. Stow personal weapon / Waffe im Fahrzeug wegstecken restores the
  original seat and carrier camera. Camera effects are not overridden.
- Both menu actions use existing UIAction handling; old enum values stay intact.
  They are unavailable during a pending reload. NPC automatic behaviour and the
  multiplayer fallback are unchanged.
- Boarding and seat swaps clear player intent. Old saves without the flag and
  newly controlled AI passengers do not silently opt into the shooting pose.
- c2281f88 and b94cdb9f exposed a delayed replay of completed boarding while the
  player was in the ordinary pose. 3ab36e6f clears that completed action at entry
  for available seats, even before optional activation. Future queued mission
  actions are not globally cleared.
- Installed 3ab36e6f: 154648 M16 and 154748 AK74 passed normal menu exit/reentry,
  initial inactive state, menu Ready, mouse firing arc/reload, menu Stow and no
  extra shot after stowing. Captures show German action labels and the restored
  original vehicle view. 154847 AK74 save/reload/lifecycle passed, including
  inactive reentry and explicit reactivation following a seat change.
- 155020 M16 additionally passed a second menu Ready after Stow and repeated
  mouse firing/reload checks. 155226 NPC control still fired without player
  menu activation. General seat pose adaptation, other vehicle profiles and
  the rest of V6 remain open.

The installed jeep-manual and jeep-ride mission hints describe the new action.
