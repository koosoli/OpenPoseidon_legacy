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

## 4. Progress

* **2026-10-04, group A (items 1–5): integrated on `dec/ai-malprave-a` (2a46f85e, 0eddd247), ledger
  `AI-PORT-A01`.**
  * radioImmediate was hand-ported onto Oli's `QueueGroupCommand`, which already skipped speech for
    player orders. Every order now acts at once, and a `_replied` flag stops a double reply.
  * The audio file `Speaker.cpp` was left untouched (owner rule).
  * Suppression's near-miss check sits on the bullet's tick segment, and
    `POSEIDON_AI_SUPPRESSION_LOG=1` logs each near miss.
  * Captured: hearing (leader turns on a hidden shooter 150 m behind) and suppression (57 / 44
    near misses at 40 / 80 m, men pinned).
* **Owner report: AI in combat stance take a long time to zero in on their target** → `AI-AIM-001`
  (to investigate; Oli's `POSEIDON_COMBAT_AUDIT` logs the aim gates).
* **2026-10-04, group C (items 10–13): integrated (ca182a04), `AI-PORT-C01`.**
  * Captured: a three-truck convoy keeps its order and stays on the winding Everon road for 1.6 km.
  * Runways, tank cornering and parachutes are built and run but not captured.
* **2026-10-04, group B (items 6–9): integrated (f50c4fa9), `AI-PORT-B01`, off by default.**
  * `class CfgAIFork { gradualSpotting = 1; hunting = 1; grenadeInterval = 5; exposedParts = 1; }`
    gives Malprave's settings.
  * aiExposed was rewritten into `SoldierOldExposed.cpp` (Malprave had it in the Cry of Fear file).
  * New `POSEIDON_AI_HUNT_LOG=1` logs hunting phases, the gate that blocks a soldier, and grenades
    thrown or refused.
  * Hunting captured working (hold → half-squad flank → back to combat). The B-on firefight, the
    grenade interval and the B-off regression runs were not done (owner stopped testing).
  * Findings carried into AI-XRAY-001:
    * A standing enemy at 130–150 m on the beach is never noticed (with or without B).
    * Gradual spotting takes ~15 s at 60 m.
    * Teleported enemies leave the 300 m hunt range.
* **2026-10-09, AI-XRAY-001 on `dec/ai-xray` (bfcdc2f9):**
  * **Grenade limit fixed.** It was checked when a soldier chose to throw but recorded only when the grenade
    left his hand, after the throw animation. It is now checked and claimed when the throw starts.
    AI3 (40 m): one grenade per group per ≥ 5 s, and 826 attempts refused.
  * **Gradual spotting fixed.**
    * It credited only the period each look passed in: 0 within 100 m (Oli's 0.5 s throttle), so 10× slow.
    * It forgot after 1 s, while looks come every 5 s when no enemy is known.
    * It now follows real time.
    * Walk-in test (AI3L with `.tw/ai/evals-walkin.txt`): a visible walking man is spotted 0–4 s after line
      of sight, and prone, still defenders after 6.6–9.7 s.
  * **The beach "never noticed at 130–150 m" is terrain.** A rise 1.5 m above the eye line hides the squads
    from each other.
  * **Test pitfall:** at mission start every unit is given what it can see at once (stock). A spotting test
    must bring the enemy into view later. The AI3/AI3M runs measure nothing for spotting.
  * **Harness:** `.tw/test-ai.ps1 -Exe <branch build>` runs a branch's OpenPoseidon.exe on the installed game
    data without deploying it. `POSEIDON_AI_SPOT_LOG=1` logs every look at an unknown target.
* **2026-10-09, AI-XRAY-002 (1047fc35), off by default:**
  * Switch on with `CfgAIFork { targetScoring = 1; }`. Tunable: `attackerMemory` 10 s, `inertia` 3 s,
    `inertiaFromPlayer` 6 s.
  * An enemy who hit the soldier in the last 10 s breaks his target lock: after 3 s on the current target,
    after 6 s if that is the player, and at once if the attacker is the player.
  * The attacker is weighed before the leader's assignment and the four-enemy scan.
  * Among other targets, one seen in the last second beats one only remembered.
  * Captured in the walk-in test: a switch to the attacker after 4.8 s on the previous target.
  * The player rules are untested.
* **2026-10-09, AI-XRAY-001 play-test continued:**
  * **Hunting** (AI3L with `.tw/ai/evals-hunt.txt`: East walks in, then holds fire and runs back behind the
    rise):
    * West's three soldiers hold at 30 s, two flank about 100 m to the last known positions at 36–37 s, and
      normal combat takes over at 57 s when they see East again. That re-sighting is why Search was not
      reached.
    * The leader never hunts.
    * Rejoining after the enemy is really gone is still unmeasured.
  * **Stock mission** (`C02Battlefields.Eden`, 180 s, branch build):
    * Everything off: no change by construction. With no `CfgAIFork`, grenadeInterval is 0 and the other
      switches are off.
    * Everything on: the battle runs to the end, with 1 grenade thrown and 20 refused, 25 hunt phase changes
      and 22 switches to an attacker. No soldier switched more than 3 times; the median was 3.1 s on the
      previous target.
    * Both runs show the pre-existing `ValidateOutVehicle` assert.
    * The target log printed re-picks of the current target as switches (41 for one soldier); it now logs
      only real changes.
* **2026-10-09, AI-XRAY-003 (d69df045): hand grenades get a fuse, and soldiers run from them.**
  * **Fuse, on by default.** Stock CWA grenades went off on contact, so there was nothing to react to. A thrown
    grenade with no `explosionTime` now gets `GrenadeFuse::handGrenadeFuseSeconds`, 4 s. Setting it to 0
    restores contact detonation.
  * **Reaction, off by default** (`CfgAIFork grenadeDodge = 1`).
    * Soldiers inside a grenade's danger radius (2× blast range + 1 m) run straight away to its edge and are held
      there until it goes off.
    * Going prone was tried first and saves nobody: blast damage depends on distance and cover only.
    * Dropped-grenade test: off, 2 of 3 killed; on, all 3 live with ≤ 0.12 damage.
* **2026-10-09, review of AI-XRAY-001..003 (owner: "triple check work").** The review was my own plus an
  independent one.
  * Fixed:
    * The grenade limit was claimed even in a pose with no throw animation.
    * A recent attacker broke the target lock even when he could not be engaged (a third target could then
      be picked); the attacker also overrode a player's explicit target order.
    * After a restart or load, mission time goes back, so the static records (grenade and spotting state,
      grenade times, hunt records) looked like they were in the future. That gave ghost grenades, blocked
      spotting and blocked grenade throws, and with `grenadeInterval` 0 (stock) an old future timestamp
      could still veto a throw. Interval 0 now means no limit and nothing is recorded, and the records are
      cleared when the clock goes back.
    * A dodge or hunt order left under the group's own Hide resumed later. All tagged orders are now removed
      (`AISubgroup::ClearTaggedCommands`), and hunting skips a soldier while he is getting clear of a grenade.
    * A target seen again after a gap got a whole check interval of credit at once.
    * The dodge re-issued its order every 0.2 s, and endlessly for an unreachable spot. It is now at most once
      a second and three times.
    * Log-only: a null dereference, and an unbounded map.
  * Known, not fixed:
    * The hand-grenade fuse is set per machine. A dev-panel slider change, or a multiplayer peer on another
      build, gives different fuses.
    * `AmmoType::thrown` depends on the hand-grenade magazine having been loaded on that machine.
  * Retested after the fixes:
    * Dropped grenade: all 3 live, damage ≤ 0.10.
    * Walk-in: spotted 0–1 s after sight.
    * Hunt: holds and flanks on both sides.
    * C02 180 s, everything on: 3 grenades, 60 refused, 24 dodges, 36 switches, 54 hunt moves. Everything off:
      no X-Ray activity at all.
    * Only the pre-existing `ValidateOutVehicle` assert.
  * Unit tests: 3946 cases; the 19 failures are all in files this branch does not touch (source audits of
    DebugOverlay.cpp, HarnessBuiltins.cpp and renderer files, image saving, network fixtures).
* **2026-10-09, AI-XRAY-004 (7db710cb), off by default** (`CfgAIFork dangerList = 1`).
  * Each soldier keeps up to 8 typed dangers (hit, grenade, corpse, gunfire, squadmate died / hit, ricochet).
    Ranges and memory are taken from CWA's own quantities (blast radius, fuse, near-miss radius, hearing).
  * Priority is weight × 10 + age in ms, and the lowest wins.
  * A danger puts the soldier on alert. One in the default watch mode with no enemy in view turns to the top
    danger.
  * In C02 every type was noted, and 70 soldiers turned to a danger.
  * **Danger search** (follow-up commit):
    * A soldier with no enemy to chase searches the position of a danger worth a look (a body, unseen gunfire,
      where he was hit from, a squadmate down): hold 5–10 s, go within 3 m, rest, rejoin. Aware is enough.
    * Test: a body 10 m away with a Guard waypoint is searched at 13 s, and he is back in formation by 23 s.
  * **Owner rule (2026-10-09):** hunting and danger searches only happen for a group on a SEEK AND DESTROY or
    GUARD waypoint. A soldier or squad with no waypoint, or on Move / Hold / Sentry / Cycle, stays put by design.
    * Grenade dodging and turning to look at a danger are unaffected.
    * Result: stock C02 no longer hunts. Its groups are not on those waypoints.
* **2026-10-09, review of AI-XRAY-004 (owner: "check over work").** Fixed:
  * Enemy and civilian bodies counted as friendly: a dead man's target side is civilian, which every side
    takes for friendly.
  * Friendly fire, friendly gunfire, falls and own grenades made dangers. Only hostile sources count now.
  * Ricochets turned soldiers sideways. They now point at the shooter.
  * A hidden shooter's position was exact. It is now a guess (0.1 × distance for a hit, 0.4 × for gunfire).
  * The player's own AI squad reacted to dangers. It no longer does.
  * Searches aborted when the danger's memory ran out, and repeated after the cooldown.
  * A grenade at the top of the list hid a danger worth searching.
  * Records left from before a restart, or at a reused address, could be picked.
  * Checked in game: an enemy body is ignored, a friendly body is searched.
* **2026-10-09, AI-XRAY-006 (bbb93088), off by default** (`CfgAIFork lookOut = 1; badCover = 1`).
  * **Look-out:** during a hold, the soldier rises for 1.5 s facing the threat, then gets his stance back.
    Shown in 2 hunt runs.
  * **Bad cover:** cover where a soldier was hit by the enemy is skipped by his side for 120 s within 3 m.
    Shown in C02: marked, then skipped 1 s and 4 s later.
  * **Not done:** cover eviction. CWA picks cover per soldier with no reservation to evict from.
  * The 3 m enemy rule is already stricter in FindHideBehind.
  * Review fixes:
    * The look-out kept wiping the leader's assigned target. It no longer does.
    * No look-out in danger or under suppression.
    * Look-outs end before a save: the hunt's state is not saved, so a soldier saved mid-look stayed standing.
    * A hit soldier leaves the bad cover himself (C02: skipped 0.1 s after the hit).
    * Bad-cover records are checked against the clock after a restart.

## 5. Tuning notes

Danger ranges and memory (AI-XRAY-004) are CWA's own, each derived from a quantity the engine already has:

| Type | Range | Memory | From |
| --- | --- | --- | --- |
| hit | 2000 m | 45 s | a shooter who hit you is in range; the cap only drops impossible values |
| grenade | 15 m | 10 s | 2 × the hand grenade's 7 m blast + 1 m; 4 s fuse + 6 s to look round |
| corpse | 50 m | 30 s | a squad's spread in contact |
| gunfire | 150 m | 15 s | CWA's hearing range |
| squadmate died / hit | 50 m | 20 / 15 s | a squad's spread in contact |
| ricochet | 40 m | 15 s | 10 × the 4 m near-miss radius; as long as heard gunfire |

Grenade noticing (AI-XRAY-003) uses each grenade's own danger radius; `CfgAIFork dodgeRadius` is an optional cap
(0, the default, means none). None of these values has been play-tested yet; tune them in C02 before turning the
switches on by default.

Mechanisms worth a ticket, highest value per cost first:
1. No shooting through friends (a muzzle ray plus offset rays).
2. Bursts by distance band.
3. Dispersion by movement state.
4. A sudden attack when entering combat unseen.
5. Lingering a few seconds after combat before searching.
