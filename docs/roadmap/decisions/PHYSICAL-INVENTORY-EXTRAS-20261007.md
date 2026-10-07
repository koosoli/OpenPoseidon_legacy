# Physical Inventory fork: additional feature review

Reviewed 2026-10-07 against the author-supplied snapshot in
`inspiration/CWR-Physical Inventory/CWR-Physical Inventory` and the current
`codex/private-release-prep-20261006` engine. The original inventory port was
selective; features outside inventory were not silently removed from this engine.
Earlier engine improvements remain in the branch ancestry.

## Included in this follow-up

- Optional absolute bearing callouts for nearby single-player AI contact reports.
  Both bearing and spoken range use the player's unit as their reference, rather
  than the freefly camera. Existing target knowledge and distant grid reports stay
  intact. Speech uses stock `at`, digit and distance clips; subtitles say `Bearing
  NNN`. The radio's existing lockstep timing and variant RNG draw are retained.
- Optional bearing readout below the aim area. North is 000, east 090, south 180,
  west 270. When a weapon is selected it follows the weapon axis.
- Optional infantry grenade-launcher ground-impact range estimate and a ground
  mark captured with the existing Reveal Target binding (normally right mouse).
  The estimate uses the current ammo's drag/gravity/launch delay, optional wind,
  fuse limit, and terrain/sea height. It refreshes at 5 Hz, with at most 500 flight
  steps per refresh. It is an approximation, not a ballistic guarantee: buildings,
  bounces, dispersion and shooter velocity are not predicted. Its distance and
  ground mark both use horizontal distance from the muzzle. Hand grenades, flares,
  tank shells and rockets do not receive this display.

All three default on in single player and can be disabled independently under
**O → K**. They save to `combat_assists.cfg` in the engine's user directory.
The inventory settings reset button also resets these options. Multiplayer keeps
its existing HUD/radio behavior and classic Gear. No additional shipped assets or
modified original game archives are needed.

## Other authored changes found

| Feature in supplied fork | Decision for current engine |
| --- | --- |
| Grid inventory, vicinity/cargo, equipment slots, weight/volume, hotkeys, settings | Already selectively integrated and audited. |
| Spring recoil with permanent aim kick, camera kick, MG bracing and 14 tuning values | Not imported. It spans aim, simulation, proxy rendering and recoil state; the current engine keeps its existing recoil. The donor also hides a firing-cadence floor in this area. A separate optional port needs burst/ADS/prone/MG and frame-rate verification. |
| Finite first-aid kits and AI self-healing | Not imported. Donor globally injects kits into loadouts and can heal before verifying that the kit still exists at animation completion. Requires consumption/ownership/save fixes before enabling. |
| Jerry cans, refuelling, siphoning and gas-station actions | Not imported. Donor's integer-litre transfer and clamped tank fill can create or lose fuel near tank limits. Requires conservative fractional transfer and cancellation/save checks. |
| Revised vehicle fuel capacities | Not imported: global gameplay/config overrides can change existing missions. |
| Weight-dependent fatigue, forced walk and mouse sensitivity; health/stamina HUD | Inventory capacity budgets are already included; these extra movement/aim penalties are not. They need an independently configurable gameplay design. |
| Historical infantry loadout rewrites, ammunition/damage/drag overrides | Not imported: broad stock/addon balance changes are not necessary for inventory and can alter authored missions. |
| Two-shell Kozlice/shared chamber changes | Not imported: intertwined with reload and ammunition semantics; current audited inventory explicitly does not enable grouped-shell behavior. |
| 4:3 optics preservation | Already supported by this engine. |
| Hardcoded M21 ART alignment/zeroing, repaired MG optics and custom click sound | Not imported: model/art-specific offsets and external repaired artwork are not a general compatibility fix. |
| Runtime arsenal crate and boat editor categorisation | Found in donor configuration injection; not imported as universal engine config replacements. Suitable later as a namespaced optional gameplay/editor addon with compatibility tests. |
| HD Jeep model replacement | Donor's registration is disabled and depends on an external M151HD addon; excluded. |
| Debug bullet impact marks | Excluded; this engine already has its own impact effects/diagnostics. |

Code attribution remains GPL-3.0-or-later with the project's Section 7 terms.
The new functions adapt the donated implementation rather than replacing newer
engine files wholesale. No donor game-art or external sound dependency was copied.
