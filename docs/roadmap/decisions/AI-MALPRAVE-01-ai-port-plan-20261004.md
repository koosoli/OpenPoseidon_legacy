# AI-MALPRAVE-01 — Port Malprave's AI fixes, and the unfinished S.T.A.L.K.E.R. (X-Ray) AI work

Recorded 2026-10-04 at the owner's request: "incorporate all other AI fixes from Malprave, list
them first"; "there was unfinished stalker AI work, make sure those unfinished steps are brought
over into the to-do".

Sources, read only:
* `G:\Malprave\*.patch` (the patch files are Malprave's history)
* `G:\Malprave\MALPRAVE-KNOWLEDGE.md` §3.4–3.5 and open item 3
* `G:\Malprave\Malprave-AI-and-vehicles-report.pdf` §3, "The S.T.A.L.K.E.R. plan: done and remaining"

Swimming (SWIM-W3-001) and terrain holes (W1) are already ported. The Cry of Fear monster AI
(about 40 `cof*` patches) is not proposed: it is for that mod's undead, not soldiers.

Ledger: umbrella `AI-PORT-001`; X-Ray to-do `AI-XRAY-001` … `AI-XRAY-008`.

## 1. Malprave AI patches to port (`AI-PORT-001`; waiting on the owner's choice)

"Clean" = the patch applies to Oli as it is (checked 2026-10-04 on `dec/sinkhole-w3-swimming`).

| # | Patch | What it does | Malprave test | Fit on Oli |
| --- | --- | --- | --- | --- |
| 1 | `aiQuickWins` | Hearing gunfire reveals the shooter; three stock bug fixes (one an inverted test that meant reports with no known spotter were never sent) | Works: a squad turned on a hidden shooter 150 m behind within 1–2 s | Clean |
| 2 | `emptyEnemyVehicle` | Vehicles with a dead crew are not threats | — | Clean |
| 3 | `radioImmediate`, `radioImmediate2` | Orders act when issued; the radio line is only the voice | — | 2 clean, 1 hand port |
| 4 | `groupLimit` | 256 groups per side (stock 63) | — | Clean |
| 5 | `aiSuppression`, `aiSuppression2` | Near misses suppress: danger, prone, pinned down, squad reactions, wider spread. Also peripheral vision, and the line-of-sight budget raised from 4 to 16 tests a frame | — | Hand port |
| 6 | `gradualSpotting` | Awareness builds over about 2 s (`spotBuildTime`, `spotInstantDist`, `spotFireFactor`, `spotForgetRate`) | Inconclusive (see §3) | Hand port |
| 7 | `aiHunting` | `AIHunt.cpp`: cover → hold → flank → search when an enemy is lost; AI-led groups only, never on scripted waypoints | Inconclusive (see §3) | Hand port |
| 8 | `aiCoverGrenades` | 5 m cover spacing, one grenade per group every 5 s, no splash fire onto squadmates | Grenade limit looked broken (see §3) | Needs 7 |
| 9 | `aiExposed`, `aiExposed2` | AI see and shoot the part of a man that shows (`exposedParts`…); off by default | — | Hand port (written on top of the CoF code) |
| 10 | `convoyRoad`, `convoyOrder` | Convoy follow points on the road; order by distance to the leader | Tested, works | 1 clean, 1 hand port |
| 11 | `tankCorners` | Tanks look further ahead and slow for bends | — | Clean |
| 12 | `runwayAxis`, `runwayHeight` | AI aircraft use the real runway for take-off, circuit and touchdown | Tested on Sahrani | Clean / needs `runwayAxis` first |
| 13 | `parachuteDamage` | A parachutist can be hit, and AI aim at him | — | Clean |

All behaviour switches are `CfgAIFork` keys, as in Malprave.

## 2. Unfinished S.T.A.L.K.E.R. (X-Ray) AI work: the to-do

Malprave reviewed OpenXRay and picked eight mechanisms. State when Malprave stopped:

| X-Ray mechanism | Malprave state |
| --- | --- |
| 1. Visibility that builds over time; range shrinking off the view axis | Mostly done: gradual spotting plus peripheral vision. Smooth shrink with the angle from the head not done |
| 2. Hearing: weapons count 10×, gunfire implies an attack, adaptive threshold | Mostly done. Adaptive threshold (a loud sound masks quieter ones for a while) not done |
| 3. Enemy scoring with switch inertia | Not started |
| 4. Squad enemy distribution | Not started |
| 5. Squad cover reservation, 5 m spacing, half-squad flank cap | Mostly done. Evicting a mate from a reserved cover and the 3 m enemy-distance rule not done |
| 6. Cover points found in advance, and the "Best" evaluator | Not started (Poseidon still scores nearby objects each time in `FindHideBehind`) |
| 7. Typed danger list | Not started |
| 8. Cover → look out → hold → flank → search | Done apart from a separate look-out (peek) step |

Each remaining step is its own patch with its own `CfgAIFork` switch, in Malprave's suggested order
(cheapest and most visible first). All need the corresponding §1 patches ported first.

* **AI-XRAY-001: play-test and fix what is in.**
  * Run the X-Ray patches (§1 items 1, 6, 7, 8) in vanilla missions and a firefight test. Check
    that waypoint missions still complete, hunters rejoin their groups, spotting times feel right by
    day and night, and grenades and splash weapons behave. Tune the `CfgAIFork` values.
  * Carry over Malprave's open test problems (§3).
* **AI-XRAY-002: enemy switch inertia and scoring (mechanism 3).**
  * When picking a target, favour whoever last hit me (−1500 for the player, −500 for others) and
    whoever is visible now (−1000).
  * Keep the current target for 3 s (6 s when switching away from the player), unless it died or a
    visible one replaces an unseen one. This stops targets flip-flopping.
  * Hook: target selection in `AIGroup::AssignTargets` / `SelectFireWeapon`.
* **AI-XRAY-003: hide from grenades** (X-Ray `hide_from_grenade`).
  * Soldiers who see or hear a live grenade land near them run from it before anything else. CWA AI
    ignores thrown grenades.
  * Needs a check for live grenade shots near each unit, then a short run order out of the blast
    radius.
* **AI-XRAY-004: typed danger list (mechanism 7).**
  * Each group keeps a short list of dangers with a position: grenade, gunfire heard, ricochet,
    fresh corpse, attacked. Priority is type base × 10 + age in ms; entries expire.
  * Aware or Combat units turn towards the top danger. Hunting can search danger positions as well
    as enemies (for example walking over to check a body).
  * Replaces CWA's single on/off "danger".
* **AI-XRAY-005: squad enemy distribution (mechanism 4).**
  * At group level, share enemies out: each goes to the member best placed to beat it, with a
    simple strength ratio standing in for X-Ray's fuzzy victory probability.
  * Scale each enemy's weight by (1 − p) so others spread onto other threats, then swap targets
    between members when both end up closer.
  * Plugs into the existing `AssignTargets`, once a second.
* **AI-XRAY-006: look-out step and cover eviction (mechanisms 5 and 8).**
  * Add the peek between cover and hold: crouch or step out of cover for 1 s, then back.
  * A mate closer to a cover spot takes it, and the other moves on.
  * Reject cover within 3 m of an enemy.
* **AI-XRAY-007: cover points and the "Best" evaluator (mechanism 6).** The largest step.
  * Precompute cover points per island on the operative grid, from object edges, stored in a grid,
    and cache them per map when a mission loads.
  * Score them as X-Ray does: reject a cover if the threat is within 30° of the route there and
    within 1.5× its distance; prefer low exposure towards the enemy.
* **AI-XRAY-008: polish (mechanisms 1 and 2).**
  * View range shrinks smoothly with the angle from the head.
  * Adaptive hearing threshold.
  * Optional: a visibility meter on the player HUD, like X-Ray's `SetActorVisibility`.

Left out on purpose, as in Malprave:
* X-Ray's wounded-enemy handling (CWA has no wounded state).
* Smart covers.
* The full GOAP planner (the per-soldier state machine gives the same chain).

## 3. Malprave's open test problems, carried into AI-XRAY-001

From Malprave job 23 (29 Sep), the first and only A/B test of the X-Ray patches:

* **Grenade interval (`aiCoverGrenades`) looks broken.** West threw 4 grenades in 1.6 s and East 3
  in 2 s; the rule is one per group per 5 s.
  * The hook is `Hunt::OnGrenadeThrown` in `EntityAI::FireWeapon` (`VehicleAICombat.cpp`, ammo
    `AmmoShotShell` with `initSpeed < 30`); the check is `Hunt::GrenadeAllowed`.
  * Suspects: the throw is decided before the shot is recorded, or the `initSpeed` test does not
    match CWA's `HandGrenade`.
* **Gradual spotting and hunting were never measured.** Enemies moved in with `setPos` from 2 km
  away were never noticed (even at 8 m) in either pass, so the teleport broke the test.
  * Retest with enemies spawned in place.
  * Add temporary logging: spot build-up and time, hunting phase changes and which gate blocks
    them, every grenade recorded or refused.
* **`knowsAbout` reads 0** even while units clearly react to an enemy, so it is no use as a
  measure.
