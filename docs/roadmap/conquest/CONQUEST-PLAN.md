# OpenPoseidon — Conquest Mode: audited plan (revision 2)

**Status:** audit and revision of the owner's draft, 2026-10-25. Nothing is built yet.
**Draft:** [OpenPoseidon_Conquest_Mode_Design_Proposal.md](OpenPoseidon_Conquest_Mode_Design_Proposal.md), kept unchanged beside this file.
**Audited against:** `perf/reforger-render-hitches` @ `d29d139c4`, the build installed in the game folder on
2026-10-25. Every `file:line` below refers to that commit. Paths are relative to `engine/Poseidon/` unless they start
with a top-level folder (`docs/`, `tests/`, `apps/`, `content/`, `dev-missions/`).

**Kept from the draft, unchanged:** the core fantasy ("one soldier in a living war, who can also become the
commander"), Everon first, an NPC-only war that runs without the player, the same rules for AI and human
commanders, no spawning at contested bases, artillery as one shared paid mechanic, the LLM as an option that is
never required, two factions before three.

---

## 0. The audit in one page

**Verdict.** The vision and the fairness rules hold, and starting small on Everon is right. Four things must
change before anyone builds from the draft:

1. **It assumes several things the engine does not do.** Single-player respawn, high command, artillery and
   modern SQF are all missing on stock Everon. Each one needs engine work, and none of it is optional (table A).
2. **It misses the engine's own strategic layer.** OFP's per-side `AICenter` (2001) already keeps a side-wide
   enemy database with fading accuracy (fog of war), a threat map, a defence allocator, a logistics dispatcher and
   an unused, already-saved `_resources` field. An AI commander built on top of it gets fog of war for free.
   Built beside it, it would duplicate all of that (table B).
3. **It leaves rules open where a first slice needs defaults.** This revision proposes concrete, tunable defaults:
   a front-line graph, the capture math, spawn safety, player redeployment, and a war score that guarantees a war
   ends (§3).
4. **Its slice order tests the riskiest question last.** Nobody knows yet whether an NPC-only war on this engine
   moves, ends and stays inside the frame budget, and every other feature depends on that answer. It is now
   Slice 0, before menus, deploy screens or commander UI (§6).

### A. What the draft assumes and the engine does not have

| Draft assumes | Engine today | Needed |
|---|---|---|
| The player respawns at a base | In single player the mission ends when the player's body dies (`World/WorldImpl.cpp:618-624`, `EMKilled`). The `selectPlayer` command switches control (`Game/Commands/GameStateExt.cpp:1511` → `World/WorldSetup.cpp:112`) but not the "real player" the end check reads, so the mission still ends when the first body dies. `forceRespawn` and `setPlayerRespawnTime` are stubs (`Game/Commands/GameStateExtA3Stubs.cpp:284, 601`). Multiplayer respawn modes exist (`Network/NetworkIface.hpp:121`), but multiplayer has no save game. | Engine: Conquest handles player death itself and shows a deploy screen (§3.5, Slice 1) |
| The commander orders many squads from the map | Map orders reach only the player's own group. `hcSelected` returns `[]` and `hcSetGroup` does nothing (`Game/Commands/GameStateExtA2.cpp:3252, 3467`). | A new map overlay with selection and orders (§4.4, Slice 2) |
| Artillery can be requested | `doArtilleryFire` and `commandArtilleryFire` are stubs (`GameStateExtA3Stubs.cpp:235, 140`). CWA's `CfgVehicles` has no artillery piece. `CfgAmmo` has `Shell73`…`Shell125` and `MortarShell`. | An abstract barrage that spawns stock shells (§3.8, Slice 3) |
| Mission scripts can use modern SQF | On stock Everon the Arma 3 commands are not registered: `params`, `pushBack`, `findIf`, hash maps, `addMissionEventHandler`, `inArea`, `enableDynamicSimulation`. They are registered only with `--arma3` (`Game/Compat/Arma1Compat.cpp:605`, `GameStateExtA3.cpp:2819`). `{…}` is code only in Arma 2/3 mode (`Arma1Compat.cpp:602`). | Rules in C++; the script surface stays in the OFP/Arma 1 dialect (§2.3) |
| Distant forces can be simulated cheaply | Dynamic simulation **freezes** the groups it marks while no player is near (`World/Simulation/DynamicSimulation.hpp`). It would stop the war wherever the player is not. At full rate, ~190 units cost 2.9–3.3 ms of a 4.7–5.3 ms tick (`docs/roadmap/decisions/PERF-013-the-ai-stage-is-the-engines-largest-cpu-cost-20260831.md`, idle re-measurement). 708 soldiers in 63 groups cost 26–50 ms per tick (`docs/roadmap/decisions/PERF-033-tiered-visible-far-sim-rate-20260914.md`). | A hard cap on live AI in v1; virtual far groups only if the cap proves too small (§4.7) |
| NPC chat is ready for players | Its only settings screen is a dev-panel tab (`Dev/NpcChat/`, settings in `npcChat.json`), and release builds refuse `--dev` (`Foundation/Platform/AppConfig.cpp:303`). | NPC chat settings in the normal Options screen, before any LLM feature (Slice 4) |
| A long war stays clean | There is no corpse or wreck cleanup. `hideBody` is ignored (`GameStateExt.cpp:1819`). | Conquest deletes old bodies and wrecks itself (Slice 0) |
| A lost base stops being defended | `createGuardedPoint` exists (`GameStateExt.cpp:2101`), but nothing removes a guarded point (`AI/AICenterImpl.cpp:544` only adds them). | A small engine addition: remove or reassign guarded points (Slice 0) |
| Capture by area control | No capture logic exists. Arma 2's "WEST SEIZED" trigger reads as plain presence here (`AI/ArcadeTemplate.cpp:183`). | A new capture rule (§3.2) |

### B. What the draft should reuse

| Existing system | Where | Role in Conquest |
|---|---|---|
| **The per-side strategic layer (2001):** a side-wide target database with fading side/type/position accuracy; a threat ("exposure") map per grid cell, split into soft/armour/air; a guard allocator that sends GUARD-waypoint groups to known enemy vehicles, then to guarded points, every 25–35 s; support dispatch for SUPPORT-waypoint repair/ammo/fuel/medic groups; and an unused `_resources` field ("resources are not used") that saves with the game | `AI/AICenter.hpp:194-368`, `AICenterImpl.cpp:333` (guarding), `AICenterImplPreview.cpp:557` (`Think`), `AICenterImpl.cpp:1787`, `AICenterStats.cpp:1097` | The AI commander's eyes (fog of war built in), its threat estimate, its reaction force, its later logistics, and the faction pool |
| The map's drawing of the side's known targets | `UI/Map/UIMapMain.cpp:1329` (debug view today) | The commander map's "known contacts" layer |
| Runtime groups: `createCenter`, `createGroup`, `createUnit`, `addWaypoint`, `setWaypointType`, `deleteVehicle`, `setFriend`. Limits: 256 groups per side, 12 units per group | `GameStateExt.cpp:2089-2442`, `Core/Config/Configuration.cpp:344`, `AI/Path/AITypes.hpp:31` | Purchases, garrisons, three-way hostility |
| Waypoint types MOVE, SAD, HOLD, GUARD, SUPPORT, GETIN, UNLOAD … | `AI/ArcadeTemplate.cpp:50-78` | The whole order vocabulary. No new tactical AI |
| NPC chat: cloud and local providers, one background worker, a situation report built from the group's *own* target list ("never ground truth"), validated orders, an HQ speaker | `Dev/NpcChat/` (`NpcChatGame.cpp:591`) | HQ conversation and the optional LLM commander |
| Flags (`setFlagSide`, `setFlagOwner`, `setFlagTexture`) and markers (`createMarker`, `setMarkerColor`) | `GameStateExt.cpp:2094-2170` | Ownership you can see in the world and on the map |
| Main-menu entries injected from C++ (ARMA CONTENT, WHOLE WORLD, ISLAND EDITOR) | `UI/OptionsUIApp.cpp:384-388` | The CONQUEST button |
| Content packs: `content/default-packs` → `assets\Mods\@OP_*`. Pack missions merge into Missions/OpenPoseidon | CLAUDE.md rule 1c; `docs/roadmap/decisions/MISSION-GROUPING-20260912.md` | How Conquest ships |
| Single-player saves: `AICenter` and SQS scripts serialise (`Game/Scripting/Scripts.cpp:369`); a key-value `db*` file API | `dev-missions/database-loadout.Intro` | Save/load; war history across sessions |
| Dev-panel Zeus: free-fly camera, click and lasso picking | `Dev/Debug/DebugOverlay.cpp:825, 871` | Selection math for the commander map; a dev view of the war |
| AI architecture rules: a planner issues existing commands, player orders interrupt, a separate saved planner RNG, stable ids, a log-only "shadow planner" first | `docs/roadmap/AI-ARCHITECTURE-ASSESSMENT-20260910.md` | Hard constraints on the commander (§4.3) |
| Harnesses: AI timeline oracle (`POSEIDON_AI_TIMELINE`, `PoseidonTools timeline diff`), pinned tick (`POSEIDON_LOCKSTEP_HZ`), harness `eval`, `setAccTime` | `AI/AITimeline.cpp`, `apps/tools/Tools/commands/TimelineCommand.cpp`, `GameStateExt.cpp:1973` | The Slice 0 soak test and determinism checks |

**A limited reference: the 2003 CTI missions.** OFP's community "Capture The Island" missions (crCTI, MFCTI, TZK)
are the same genre, and the upstream suite still boots them (`tests/integration/multiplayer/crcti_*`,
`mfcti_dedicated_init.test`). The owner wants Conquest far more modern than they are, so they are a reference
only for *mechanics that proved workable on this engine*: for example MFCTI Coop's town garrisons that spawn only
when players approach (a cheap answer to §4.7's far-group problem) and truck-built forward outposts as respawn
points. They are not a reference for UI, feel or structure. MFCTI's SourceForge page states no license, so none
of its code may be reused. The main reference is Arma Reforger's Conflict, built with modern UX: HUD capture bars,
a deploy map, readable radio and war-log feedback, and commanders that respect fog of war.

---

## 1. Vision

Unchanged from the draft (§1–§2 there). In short: choose Conquest, choose a side, and enter an island-wide war run
by NPC commanders. Bases change hands, income buys reinforcements, and reinforcements appear only at safe bases.
The player fights as a soldier and can take command at any time.

## 2. Principles this revision adds

1. **The referee and the commanders see different things.** The rules engine (the *referee*) uses ground truth:
   who is physically inside a capture zone, whether an enemy stands inside a base. Every *commander* — Standard
   AI, LLM or the human — sees only its side's knowledge: the `AICenter` target database, its own units, and the
   objectives as its side knows them. A commander hears referee results only the way its side would: "Montignac
   is under attack", "purchase refused: base contested". This makes "the AI does not cheat" checkable: the
   commander code has no access to the world's unit list.
2. **One intent pipeline.** All commander types emit the same small set of intents (§4.2). The referee validates
   and executes them and gives a reason when it refuses one. Nothing else spawns or orders Conquest units. The
   draft wanted this; here it becomes the architecture.
3. **An engine core, data in config, a thin script surface.** The rules, commanders, UI and saving go in C++
   (`Game/Conquest/`, picked up by the existing `Game/*.cpp` glob), unit-tested without a world. Objectives,
   rosters, costs and rates go in a config class, so a mission maker can retune a war or build another island
   without C++. Missions get a handful of SQF commands and one event hook. Why C++: four of the blockers in table
   A need engine work anyway, C++ state saves through `ParamArchive`, the `AICenter` and NPC chat are C++, and
   stock-Everon SQF lacks the Arma 3 conveniences.
4. **N factions in the data model, two in the first slices.**
5. **Budget first.** The live-AI cap is set from measurement. A purchase is limited by money *and* the cap, and
   the cap is shown to the player as a reinforcement count, not hidden.
6. **No deadlocks by construction.** An HQ cannot fall while its side still owns another objective. The HQ always
   earns a minimum income. The player can always deploy somewhere. A war score guarantees the war ends.
7. **Inert unless asked.** Conquest code does nothing unless the mission declares `CfgConquest`, so stock missions,
   menus, saves and AI stay exactly as they are.

---

## 3. Rules: proposed defaults

Every number below is a config value and a **first guess, to be tuned from Slice 0 soak logs**.

### 3.1 Objectives and the front line

- An objective has a name, a centre, a capture radius (town 120 m, airfield 200 m), an income value, a kind (HQ,
  town, airfield, depot), a list of spawn spots (behind buildings, never the open centre), and its neighbours.
- The objectives form a graph. **A side may capture only an objective adjacent to one it owns.** This keeps the
  fighting on a front where the player is likely to be, removes the lone back-capper exploit, gives the AI
  commander a short list of choices, and protects HQs without special rules. Reforger's radio-range rule serves
  the same purpose.
- In v1, neutral objectives start empty. Resistance garrisons come with the third faction.

### 3.2 Capture

- The referee ticks at 1 Hz. A side's *strength* inside the radius: each living, non-captive man counts 1 (crew
  inside vehicles too), and a crewed armoured vehicle adds 2. Empty vehicles and aircraft more than 30 m above the
  ground count 0.
- A side makes progress only while its strength is **at least twice that of every other side present**. Otherwise
  the objective is *contested* and progress freezes. Why 2:1 and not "any enemy freezes it": under that rule, one
  AI soldier stuck in a house holds off a whole squad indefinitely, which is both an exploit and a classic AI
  stalemate.
- Rate: a full bar takes `90 s / clamp(strength / 4, 0.5, 2)`, i.e. 45 s for an eight-man squad and 180 s for two
  men. Piling on more troops cannot beat the 2× cap.
- An owned objective must first be **neutralised** (bar to zero), then filled for the new owner, so a full capture
  against a defender takes twice as long. Progress left behind decays back at a quarter of the rate.
- On capture: the flag and marker change, income starts next tick, and spawning there unlocks after a 60 s settle.

### 3.3 Economy

- One resource: the **faction pool**, stored in `AICenter::_resources` (already saved).
- Income per minute is the sum of the owned objectives' values: HQ 10 (the floor), town 5, Montignac 10.
  Starting pool: 300.
- Costs: rifleman 10, rifle squad (8) 80, AT team (4) 60, jeep 20, truck 30, APC 120, tank 300, artillery barrage
  150. Money is taken when the unit spawns, not when it is ordered.
- **Live-AI cap per side:** start at 60 (≈120 total plus the player). Raise it toward 100 only if the 80/160/240
  measurement in §4.7 allows. A purchase over the cap is refused with "manpower cap reached".
- No upkeep in v1.

### 3.4 Purchasing, spawning and spawn safety

- A purchase names an item and a base. The base is **eligible** when it is owned and not contested, no enemy man
  or vehicle is within the security radius (400 m), no friendly unit was hit inside that radius in the last 60 s,
  and the base kind allows the item (vehicles at HQ or depot; aircraft only at the airfield, later).
- An ineligible order waits at faction level for up to 120 s, can be redirected by its commander, and is then
  refused with the reason.
- The safety check deliberately uses ground truth. That tells a commander "an enemy is near Montignac" when its
  base is blocked. Accept it and present it as the garrison's own contact report, the same as Reforger's "base
  under attack".

### 3.5 Player death and redeployment (single player)

- In Conquest the mission does **not** end when the player dies. Conquest takes over the single-player death check
  (`World/WorldImpl.cpp:618`) and shows a deploy screen: the map, the eligible bases, the cost and a timer.
- The player redeploys as a new soldier after 15 s, paid from the faction pool at the price of one rifleman. If the
  pool cannot pay, deploying at the HQ is free after 60 s. The player is never locked out.
- The transfer must set the real player, the camera and the AI brain, not just the control switch `selectPlayer`
  does today (table A).
- The war is lost when the side owns no objective.

### 3.6 The player's squad

- The player leads a normal squad created by the faction, as in OFP's own campaign. HQ assigns it objectives like
  any other group (a radio message plus a waypoint), and the player may ignore them. The commander counts that squad
  in its plans but never relies on it alone. This connects the soldier to the war with no special case, and it is
  how dynamic missions (draft §16) appear for free.

### 3.7 Victory, stalemate and restart

- **Victory:** the enemy owns nothing (its HQ falls last, by §2.6).
- **A guaranteed end, the war score:** each side starts at 500. Each minute, the side holding more non-HQ
  objectives drains the other by 5 × the difference. Every man lost costs 1 point and every vehicle 3. At 0 the side
  is defeated. A 3–2 split ends the war in about 100 minutes, a 4–1 split in about 35, sooner with casualties. An
  optional time limit (default off) awards the war to the higher score.
- **Restart:** the end screen offers "New war" (same settings, new seed) and "Main menu". Each result is appended
  to a `db*` war-history file in the profile.

### 3.8 Artillery: one shared support action

- Intent: `Support{barrage, target, radius}`. Requirements: an owned HQ or airfield within 4 km of the target
  (positions matter; Everon is 12.8 km wide), the cost, a 5-minute cooldown per side, and a target the side
  actually knows: a player marker, a point the player names, or a known target from the side's database.
- Execution: after 30 s of flight time, 6–8 stock `Shell120` rounds land over 20 s with a dispersion of 40 m
  when a friendly unit has line of sight to the target within 1 km, and 120 m otherwise. Friendly groups within
  300 m get a radio warning when the mission is accepted.
- The three request paths share this one intent: a soldier asks HQ over the radio menu (later in chat); the
  Standard AI uses it on defended objectives it is attacking; the human commander clicks the map.

### 3.9 Three factions

- The third faction is Resistance (CWA has the units). Relations are set with `setFriend`: all hostile by default,
  with a config option for "Resistance friendly to West". Three-way capture uses the same 2× rule against the
  strongest other side present.

### 3.10 Logistics, later

- Physical supply through the existing SUPPORT system: ammo, repair and fuel trucks on SUPPORT waypoints already
  serve groups that ask for help (`AICenter::UpdateSupport`). Supply convoys that raise a base's income, and radio
  relays, come after the core loop is fun.

---

## 4. Architecture

### 4.1 Modules (`Game/Conquest/`)

| Module | Responsibility |
|---|---|
| `ConquestState` | Objectives, owners, progress, pools, cap counters, war score, war log. Pure data plus `Serialize`. |
| `ConquestRules` | Capture step, income, eligibility, victory: pure functions over the state and a *presence snapshot* (strength per objective per side). Catch2 tests run without a world. |
| `ConquestReferee` | Game-thread glue: builds the presence snapshot at 1 Hz, applies the rules, executes intents through the existing group and waypoint paths, spawns shells, cleans up, sets flags and markers. |
| `ConquestCommander` | Interface `Plan(const SideView&) → intents`. `SideView` is built only from the side's `AICenter` targets, its own units, the objectives as the side knows them, its pool and cap, and recent intent results. Implementations: `StandardCommander`, `HumanCommander`, `LlmCommander`. |
| `ConquestUi` | HUD capture bar, deploy screen, commander map overlay, setup screen, end screen, dev-panel tab. |
| Config | `class CfgConquest` in the mission's `description.ext` (or a pack config): objectives, graph, rosters with class names and costs, rates, caps, commander type per side. |
| Script surface | `conquestOwner`, `conquestProgress`, `conquestPool`, `conquestIntent`, plus an `onConquestEvent` hook. Kept deliberately small. |

### 4.2 Intents

| Intent | Meaning |
|---|---|
| `AssignObjective(group, objective, capture \| defend)` | The main order |
| `Move(group, position)`, `Hold(group)` | Direct control |
| `Buy(item, base)` | Purchase at a base (§3.4) |
| `Support(kind, position)` | Artillery (§3.8) |
| `SetPriority(objective, weight)` | Steers a planner (Hybrid LLM, human requests) |
| `Release(group)` | Hands a human-locked group back to the AI |

Each intent comes back *accepted*, *queued* or *refused (reason)*, and goes into the war log, which feeds the debug
tab, the radio lines and the LLM's memory.

### 4.3 The Standard commander

Every 20 s (± a jitter from its own saved RNG), and immediately on events (objective lost or taken, base
blocked, group destroyed):

1. **Score the frontier.** Value + proximity to own HQ − estimated enemy strength near the zone (from the side's
   `AICenter` targets, weighted by their fading accuracy) − the threat-map value. Own objectives under threat get a
   defence score.
2. **Assign groups.** Send the nearest free groups to the best attack target until the estimated strength is at
   least 1.5× the known enemy. Keep one reserve group per front on GUARD, and register owned objectives as guarded
   points: the 2001 guard allocator then answers contacts on its own.
3. **Buy.** If the pool and cap allow, replace losses at the eligible base nearest the main effort.
4. **Detect stalls.** A group that has made no progress toward its objective for 4 minutes, or has lost more than
   half its men, falls back to the nearest owned objective and merges with another group.

Orders go through waypoints (MOVE, then SAD at the objective; HOLD or GUARD to defend), never as per-unit
destinations, and player orders always interrupt, as the AI architecture assessment requires. Personalities
(aggressive, cautious) are weight sets in config. The first version runs as a shadow planner that only logs its
intents, then gets switched on.

### 4.4 The human commander

- **Take command** and **Step down** from the radio menu or the map. In single player the AI commander stays on as
  an *adjutant*: a group the player orders is locked to the player for 10 minutes or until its task completes,
  and every other group follows the AI plan. This avoids micromanagement and keeps the war moving while the player
  is back at the rifle.
- **Map overlay (2D first, on the existing map):** objectives with owner, progress and eligibility; own groups as
  icons with strength and task; known contacts from the side's target database, with their age; click and box
  selection (the Zeus lasso math); a right-click order menu; a purchase panel per base; an artillery tool. A 3D
  overhead camera can come later if the 2D map proves too limited.
- **Handover** in either direction is trivial: the state lives in the referee and every commander speaks intents,
  so switching only changes who produces them.

### 4.5 The LLM commander (optional)

- It is built on NPC chat's providers and worker. The `SideView` becomes a compact report, and the model answers
  with intents as JSON against a schema given in the prompt. The answers are validated like any other intent.
- **Default mode: Hybrid.** The LLM sets objective priorities and posture, and may suggest purchases. The Standard
  commander does the assignments and everything time-critical. Done that way, the LLM can never stall the war and
  its output stays small. "Full LLM" (issuing `AssignObjective` directly) is an experiment, not the default.
- **Cadence:** on events and every 4 minutes, one request in flight per side. A proposal older than 60 s, or one
  that refers to state that has changed since, is dropped. On a timeout or a failure the Standard commander simply
  keeps going; it never stopped.
- **HQ conversation:** the existing HQ speaker gets the `SideView`, the current plan and its reason strings, so
  "Why aren't we attacking Montignac?" is answered from the real plan. A player's request becomes a
  `SetPriority` intent that the commander can accept or refuse, and it says which.
- **Prerequisite:** NPC chat settings in the normal Options screen, reading and writing the same `npcChat.json` the
  dev tab uses: one settings struct, two views. The draft's "one shared configuration" requirement holds by
  construction, because there is only one store today.
- **A different model per side** is one config field once the above exists.

### 4.6 Saving

- Single-player save: the Conquest state, the commanders' state (plans, locks, RNG) and the intent queue
  serialise through `ParamArchive` next to `AIGlobalSerialize` (`AI/AICenter.hpp:608`). Test: save mid-capture,
  load, and the timeline oracle shows progress continuing identically.
- War settings and results go in a `db*` file in the profile. Persisting world damage stays out of scope, as the
  draft already suggests.

### 4.7 Performance budget

- Referee plus commanders: at most 0.3 ms per tick on average (1 Hz work, spread out).
- **Set the live-AI cap from measurement:** run Everon Conquest soaks at 80, 160 and 240 live AI at 60 Hz with the
  player at the front (the AI architecture assessment's 80/160/320 recipe), and keep the largest cap whose
  simulation stage stays within budget. The AI architecture assessment already records a frame-rate complaint at
  "80+ soldiers", so do not assume more.
- Never mark Conquest groups for dynamic simulation; they would freeze. PERF-033's visible-far tiering already
  applies.
- Cleanup: dead bodies older than 5 minutes and more than 500 m from the player, out of view; wrecks after 10
  minutes.
- **Version 2, only if the cap proves too small for a credible war:** virtual far groups. A group far from the
  player despawns into a token that travels the road graph and fights other tokens abstractly, and it respawns at
  least 1.5 km from the player, out of view. This is the hardest part of the whole plan, and it is deliberately not
  in the first slices.

### 4.8 Multiplayer, later

The first version is single-player, as the draft says. Keep the referee authoritative and the only writer of
Conquest state, and keep the UI reading only the side view. A hosted session can then later run the referee on the
server and replicate the side views; the multiplayer respawn modes already exist. Multiplayer has no save game, so
persistence there would move to `db*`.

---

## 5. Everon: a first layout to try

Positions are from `CfgWorlds >> Eden >> Names` in CWA's config, in map metres (x east, y north). The airfield
position is the `ilsPosition`. **None of this has been checked in game yet** for roads, cover or spawn spots.

| Objective | Kind | Position | Neighbours |
|---|---|---|---|
| Airfield (Saint Phillippe) | East HQ | 4772, 10923 | Tyrone |
| Tyrone | town | 4946, 9056 | Airfield, Montignac, Gravette |
| Gravette | town | 4112, 7772 | Tyrone, Montignac |
| Montignac | town, double value | 4935, 6994 | Tyrone, Gravette, Entre Deux, Figari |
| Entre Deux | town | 5735, 7059 | Montignac, Figari |
| Figari | town | 5283, 5375 | Montignac, Entre Deux, Morton |
| Morton | West HQ | 5134, 3955 | Figari |

Morton → Figari 1.4 km, Figari → Montignac 1.7 km, Montignac → Tyrone 2.1 km, Tyrone → airfield 1.9 km. Gravette
and Entre Deux are flanks 0.8–1.1 km from Montignac. HQ to HQ is 7 km: walkable fights where trucks still matter.
The third faction could later start on the west coast at Lamentin (1333, 5962) or Le Moule (2590, 5384) and meet
the front at Gravette and Figari.

---

## 6. Slices (replaces the draft's §14)

Each slice ends with checks that can be measured, and says what it leaves out.

### Slice 0: the war runs with nobody watching (go/no-go)

**Build:** `ConquestState`, `ConquestRules`, `ConquestReferee` and `StandardCommander` for two factions;
`CfgConquest` for the §5 layout; a dev mission `dev-missions/conquest-everon.eden` with HQ garrisons; a dev-panel
tab (objectives, pools, plans, war log); body cleanup; guarded-point removal; rules unit tests.

**Done when** (harness runs, the player captive as a spectator, three seeds):
- objectives change hands at least 6 times an hour, and no side takes every neutral objective in the first 10
  minutes (no snowball);
- the war ends by score or HQ within 3 hours, or the war log says exactly why not;
- live AI never exceeds the cap, and the simulation stage stays inside the budget set by the §4.7 measurement;
- no script errors, and no group sits stuck for more than 5 minutes without being reassigned;
- the commander code reaches the world only through `SideView` (enforced by the API and checked by a test).

**Not included:** player respawn, UI beyond the dev tab, the menu, artillery, LLM.
**Owner checkpoint:** a 10-minute capture of a soak run, before Slice 1 starts.

### Slice 1: play it as a soldier

The death handler and deploy screen; HQ tasks for the player's squad; a HUD capture bar with a contested state;
flags and markers; radio lines for captures, losses and purchases; an end screen with "New war". Ship it as an
`@OP_Conquest` pack (Missions/OpenPoseidon), plus a CONQUEST main-menu button that launches it (setup = choose a
faction).
**Done when:** the player can die five times in one war and always redeploy; a save/load mid-capture restores the
progress (timeline oracle); the owner plays 30 minutes.

### Slice 2: take command

Take command and step down, the map overlay, click and box selection, orders, purchases, priorities, adjutant locks.
**Done when:** handover in both directions mid-war keeps orders, pools and ownership, and a whole war can be run as
commander with the mouse alone.

### Slice 3: the full war

Artillery; Resistance as the third faction; setup options (difficulty, starting pool, commander type per side, war
score); a balance pass from the soak logs; the war history.

### Slice 4: the optional LLM

The NPC chat section in Options (same `npcChat.json`, available in release builds), the Hybrid commander, HQ
conversation from the Conquest state, a model per side, and failure-injection tests (timeouts, invalid JSON,
provider down) in which the war must carry on unchanged.

### Slice 5: scale and other worlds

Virtual far groups if the caps from Slices 0–3 are too small. Objectives generated from `CfgWorlds` names for
Malden, Kolgujev and Nogova (the Arma 2 location commands already read them: `GameStateExtA2.cpp:2159`). Then the
imported worlds.

---

## 7. Tests

- **Unit** (Catch2, `PoseidonTests "[conquest]"`): capture math including contest, decay and three-way; income;
  eligibility; victory and score; intent validation; a serialisation round trip.
- **Integration** (Trident, `tests/integration/conquest/`): boot the dev mission, run N minutes, assert the state
  through the `conquest*` commands via `eval`.
- **Soak:** the AI-only war, with a metrics CSV (captures, pools, live AI, simulation ms) over three seeds.
- **Determinism:** two same-seed runs compared by the AI timeline oracle.
- **Edge cases:** save/load mid-capture; handover mid-assault; base blocked while a purchase waits; last objective;
  player death with an empty pool.
- **Regression:** stock missions unaffected; Conquest stays inert without `CfgConquest`.

---

## 8. Defaults this plan picks (the owner can override any)

- The mode keeps the name **Conquest**.
- A **war score** guarantees the end (§3.7).
- The player **leads a squad** rather than joining one as a rifleman (§3.6).
- The front-line graph (§3.1) and the 2:1 capture rule (§3.2) instead of free capture and "any enemy freezes it".
- Hybrid as the default LLM mode (§4.5).

## 9. The draft's extra ideas, sorted

| Idea (draft §16) | Verdict |
|---|---|
| Dynamic missions | Falls out of HQ tasks for the player's squad (Slice 1) |
| Morale and surrender | Cheap later: `allowFleeing`, `fleeing`, `setCaptive` exist |
| Recon and intelligence | Already in the engine: the side database's fading accuracy; scouts raise knowledge |
| Squad experience | Later: units already carry experience and rank |
| Supply routes | Later, through the SUPPORT system (§3.10) |
| AI personality | Slice 3, as weight sets |
| Civilian dynamics | Out of scope |
| Spectator mode | Slice 0 needs it anyway (dev tab + free camera) |
| Seeds and presets | Slice 3 |
| Better UI feedback | Slice 1: the war log drives the radio lines |

---

## Appendix: what changed against the draft

- **§5 (spawning):** "safe" is defined (400 m ground truth, hit memory, settle time); an order waits, can be
  redirected and is then refused, and money is taken only on spawn.
- **§4 (capture):** the open questions are answered with defaults: 2:1 strength, a capped rate, neutralise-then-capture,
  decay, front-line adjacency.
- **§6 (commander):** the "application" becomes take/step down plus the adjutant, because single player has no one
  to apply to; RTS controls are new map UI because high command is a stub.
- **§7 (AI commanders):** built on `AICenter` instead of beside it; hierarchical simulation moves to version 2
  behind a measured cap.
- **§8 (LLM):** Hybrid is the default; the Options screen is a prerequisite, not polish.
- **§10 (artillery):** concrete requirements and execution with stock shells.
- **§12 (victory):** a war score so no war can stall forever; HQ falls last.
- **§13 (scope):** a concrete Everon layout with positions from the game's own config.
- **§14 (slices):** reordered so the NPC-only war (the go/no-go) comes first; each slice has checks.
- **New:** single-player death handling, body cleanup, guarded-point removal, the referee/commander information
  split, the stock-Everon script dialect limits; CTI kept as a mechanics-only reference.

---

## Status, 2026-10-25: Slice 0 runs, commander control pulled forward

Built on `claude/domination-mode-ccede5` (base `perf/reforger-render-hitches` `d29d139c4`).

- **In the engine:** `Game/Conquest/` (rules, Standard commander, referee), the dev-panel "Conquest" tab,
  `conquestState` and `conquestMapClick` script commands, and `dev-missions/conquest-everon.Eden` with the §5 layout.
  13 Catch2 cases (`PoseidonTests "[conquest]"`) pass.
- **An engine bug found on the way and fixed separately:** groups created at runtime never elected a leader
  (`AIGroup::Leader()`'s fallback hid the missing election), so they ignored `move` and waypoints. See commit
  "AI: groups created at runtime elect a leader again".
- **Soak** (`scripts/Run-ConquestSoak.ps1`, report: `scripts/conquest_soak_report.py`), one seed, 4× time:
  57 game minutes, 7 captures + 2 neutralisations (9.4 changes of hands per hour), 206 dead, WEST won when EAST's
  score reached 0. Live AI stayed within the cap of 60 per side, 1 stall, referee tick about 0.03 ms (217 ms once,
  when 80 soldiers spawn at the start). Checks still open: three seeds; the simulation-stage budget measured at
  80/160/240 live AI; balance (WEST won the one run).
- **Pulled forward at the owner's request (2026-10-25):** an observer layer (every squad on the map, labelled with
  its task and strength) and first commander control. Click your squads on the map (Shift adds), then click an
  objective to attack or defend it, or a point to move. The AI leaves those squads alone for 10 minutes.
  Verified through the harness; not yet played by hand.
- **Changed from the plan:** guarded points are not used yet (the Standard commander's own defence logic was
  enough), so the guarded-point removal was not needed for Slice 0.
