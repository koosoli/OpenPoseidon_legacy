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
- A side makes progress while it has **more soldiers in the zone than every other side present** (owner,
  2026-10-26; the first version asked for twice as many). Only a tie is *contested* and freezes the bar, and the
  speed follows the margin: 8 against 3 captures like 5 alone.
  (Superseded reasoning of the first version:) Previously a side made progress only with at least twice the strength
  of every other side present. Otherwise the objective was *contested* and progress froze. Why 2:1 and not "any enemy freezes it": under that rule, one
  AI soldier stuck in a house holds off a whole squad indefinitely, which is both an exploit and a classic AI
  stalemate.
- Rate: a full bar takes `90 s / clamp(strength / 4, 0.5, 6)`: 180 s for two men, 45 s for an eight-man squad,
  22.5 s for two squads and 15 s from three squads on (owner, 2026-10-26: more soldiers capture faster). Beyond 24
  soldiers it gets no faster, so no blob takes a town instantly.
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

## Status, 2026-10-26: the commander view, vehicles, taking control

At the owner's request (2026-10-25/26), ahead of the slice order:

- **The commander view (Ctrl+M)** is a top-down strategy screen over the live war (`Game/Conquest/ConquestView.*`).
  Controls: WASD or the screen edge pan, the wheel zooms, and Q/E or the middle mouse button turn. Left click or a
  dragged box selects squads; right click orders them (an objective: attack or defend it; a known enemy: engage it;
  the ground: move there). It has its own player-facing look (Bahnschrift and Segoe UI, dark glass cards, side
  colours), not the dev style:
  - a top bar with the side's supplies and income, objectives, soldiers and the cap, both war scores and the clock;
  - squad cards with callsigns (Alpha, Bravo ...), type, task and strength pips;
  - deploy tiles per base, with price and the reason a squad cannot be bought;
  - key-cap hints and order toasts.
  Enemies show only as the side knows them, fading with age. While the view is open, the AI stops buying for the
  player's side, so the supplies are the player's; the AI keeps running every squad the player has not ordered.
- **Vehicles:** `class Squads` in CfgConquest defines what a side can buy, with `<SIDE>vehicles[]` per type. Everon
  has the rifle squad, motorised squad (truck), recon jeep, mechanised squad (M113 / BMP) and air assault squad
  (UH-60 / Mi-17). A squad mounts for moves over 200 m and dismounts near its destination. The AI buys by distance to
  the front: air beyond 2.5 km, motorised beyond 900 m, armour when rich. Figari now falls after about 3–4 game
  minutes instead of 9.
- **Squads move independently:** spare squads spread over the best three front targets, each to the one furthest
  short of its need, instead of one column to one town.
- **Taking control (optional: `possession = 0` or the HUD switch):** "Take control" puts the player in the selected
  squad leader's body. Ctrl+M returns to command, and that soldier goes back to the AI. If the soldier dies, the
  player is back in command instead of the mission ending (the single-player death check asks
  `Conquest::OnRealPlayerDead`).
- **Capture speed rises with numbers:** `90 s / clamp(strength / 4, 0.5, 6)`.
- New script commands: `conquestCommanderView`, `conquestBuy [base, type]`, `conquestTakeControl id`.
- Verified through the harness (captures in `build/conquest-runs/`, not committed); not yet played by hand.
- **Soaks with vehicles, 2026-10-26** (4× time, `scripts/conquest_soak_report.py`):

  | Seed | Game min | Changes of hands per hour | Stalls | Winner |
  |---|---:|---:|---:|---|
  | 11 | 49 | 13.5 | 8 | EAST |
  | 22 | 54 | 7.8 | 24 | EAST |
  | 33 | 51 | 12.9 | 1 | WEST |
  | 44 | 70 | 14.5 | 0 | WEST |

  Seeds 33 and 44 ran after two fixes. First, a squad in contact with known enemies within 400 m counts as busy,
  not stalled: before, squads fighting outside a held town were re-tasked into the same fight again and again.
  Second, the AI no longer buys the 4-man recon jeep (`ai = 0`); before, 24 of 32 AI purchases were jeeps. Every war
  ended, and the live cap held. Balance is not established yet: over all five runs, WEST won 3 and EAST 2.

## Status, 2026-10-26: playable from the main menu

- **Start as a soldier** (`startAs = "soldier"` in CfgConquest, with `possession` on; the default, `"commander"`,
  keeps the captive observer at Le Moule). When the war begins, the player takes over the first squad of their side;
  Ctrl+M opens the commander view, and a soldier killed goes back to command, from where another squad can be taken.
- **A soldier's HUD** while the commander view is closed: a war bar (both scores, objectives held, the clock), a card
  for the zone the player stands in with its capture progress, and banners for captures, losses and purchases.
  It hides whenever a menu, the map dialog or the pause screen is up (`World::IsSimulationEnabled()`;
  `GWorld->GetOptions()` is the main display and is set during play, so it cannot tell).
- **Shipped as a default content pack, `@OP_Conquest`** (`content/default-packs/conquest`, built by
  `scripts/Build-Conquest.ps1` from `Build-DefaultContent.ps1`, installed by `Deploy.ps1`). It carries the
  `op_conquest` addon and the mission in `Missions\OpenPoseidon\conquest-everon.Eden`, which moved there from
  `dev-missions/`. The main menu has a **CONQUEST** entry that starts it; without the pack it says so.
- **Saved games carry the war** (`Conquest::Serialize`, called from `World::Serialize` after the scripts): objectives,
  pools, scores, losses, every squad record with its group link, tracked units, the commander's body while the
  player is inside a soldier, and the war log. A load continues the saved war; a save without one (the
  start-of-mission autosave a retry loads) starts the war afresh instead of stopping Conquest. Saved with the
  commander view open, the view opens again. `scripts/Test-ConquestSave.ps1` saves mid-war, plays on, loads and
  checks that the restored state matches the save exactly and that the war continues: all 20 checks pass.
- **Engine fix found on the way:** a load read a mission's `description.ext` and stringtable from `missions/` whatever
  folder the mission was in (`World::Serialize` called `SetMission` before restoring the subdirectory). Any save of
  a content-pack or `--test-mission` mission loaded without its description.ext.

## Status, 2026-10-26 (later): the owner's play-test list

From the owner's first hand-played session:

- **Death never ends the war while the side has soldiers.** Man::Killed ran the game's own death camera
  (`scripts\onPlayerKilled.sqs`) before the World's end check that Conquest had hooked, so the player saw the
  game-over sequence. `Conquest::OnRealPlayerDead` is now asked there first: with the commander view, a possessed
  soldier's death returns the player to command; otherwise the player carries on in the next soldier of their
  squad, else the nearest squad. Only a side with nobody left ends the mission.
- **A setup screen** (`setup = 1`): side (BLUFOR / OPFOR), start as a squad leader or in command, and the options:
  commander view on/off, fog of war, taking control of soldiers, HQ radio. During the war the commander view's
  **Options** button changes the last three. Scripts start the war with `conquestStart [side, start, fog, "noview"]`.
- **Fog of war is always on**, in one of two modes: *Contacts* (the default: enemies only as the side's target
  database knows them) or *Shroud* (also: 100 m cells no friendly soldier has been within 350 m of stay black in
  the commander view, with soft edges). The enemy's 3D models are still drawn: `hideObject` is a stub in this engine,
  so hiding them needs renderer work.
- **HQ radio:** while the player does not command, the AI commander's orders go out on the side channel ("Bravo and
  Delta, attack Figari."), captures and losses are announced, and the player's own squad gets an order of its own,
  shown on the soldier's HUD with bearing and distance.
- **Command posts:** every objective gets a tent (`postClass`, default `Camp`) on free ground near its centre, with
  the owner's flag beside it (`usa_vlajka.pac` / `rus_vlajka.pac`). Only soldiers within `captureRadius` (30 m)
  of the post count; squads are sent inside that circle. The town radius still governs spawning and threat.
- **Commander view:** zoom from 3 m to 3.5 km and tilt from 2° (ground level) to 88° (R/F or the middle mouse
  button); Ctrl + left drag moves the view; clicking a soldier selects him (and his squad), the camera follows him,
  the wheel closes in without entering him (vehicles: outside their bounding sphere), and "Take control" becomes
  that soldier (T). Squads have their own shade of the side's colour (blues, reds, greens), and units carry small
  APP-6-style symbols above them: friendly rectangles, hostile diamonds, infantry, motorised, armour, mechanised,
  rotary wing.
- **Mortars:** SUPPORT in the deploy panel: 8 rounds of 120 mm on a clicked point, 60 supplies, 120 s to reload
  (`barrage*` in CfgConquest; `conquestBarrage [x, y]`).
- **Tanks and gunships:** `Armour` (M60 / T72) and `Gunship` (Cobra / Mi-24) squads; their crews stay mounted. The
  AI commander makes every fourth purchase heavy and saves up for it unless short of men.
- Checked by `scripts/Test-ConquestFeatures.ps1` (three runs: soldier deaths, command with shroud, the barrage),
  all checks pass; captures in `build/conquest-runs/features-*`.

### AI cost at 80 / 160 / 240 soldiers (2026-10-26)

`scripts/Measure-ConquestAiCost.ps1` (build at 4604f6a7a, dist, 1280x720, quality preset 1, no frame cap): the cap is
raised to half the total per side with endless supplies, and 12 frame-profiler samples are taken once the war holds
85 % of the total.

| Target alive | Mean alive | sim:step avg / p95 ms | frame avg / p95 ms | referee tick ms |
|---:|---:|---:|---:|---:|
| 80 | 72 | 0.46 / 1.55 | 6.4 / 11.9 | 0.14 |
| 160 | 151 | 1.03 / 2.53 | 12.2 / 14.5 | 2.66 |
| 240 | 156 (did not fill) | 4.23 / 13.07 | 14.5 / 26.4 | 1.29 |

The simulation stage stays far inside a 60 fps frame at 150 soldiers. The 240 run never got past 156 alive within
8 minutes (the AI buys one squad per plan and losses keep pace), so the 240 point is still open: it needs a faster
fill (a scripted spawn) rather than the war's own purchasing.

## Status, 2026-10-26 (afternoon): multiplayer, minimap, arsenals

- **Multiplayer (server-authoritative, `ConquestRefereeNet.inc`).** The server (dedicated or hosting) runs the war;
  once a second it publishes it as `conquestNet` (publicVariable: objectives, pools, scores, squads as group
  references, each side's known contacts, HQ's radio lines, the log) and every client rebuilds its referee from it,
  so the HUD, the commander view, markers and shroud work unchanged. A client's commander actions go to the server
  as `[player, kind, args] remoteExec ["conquestRequest", 2]` (order, buy, barrage, hold command) and the server acts
  for the asking unit's side. Players' slot groups are adopted as squads and get HQ's orders. No body swapping in
  multiplayer: death is the mission's respawn (`respawn = "BASE"` at `respawn_west` / `respawn_east`). Mortar rounds
  are network objects (`NetworkManager::CreateVehicle`, VLTFast), so every machine sees them and only the server
  deals their damage. The mission: `MPMissions/conquest-everon-mp.Eden` (8 slots a side), sharing the war through a
  copy of `conquest.hpp` that `Build-Conquest.ps1` checks against the single-player one.
  - Verified on a real dedicated server (`tests/integration/multiplayer/conquest_dedicated.test`, retail data): the
    war starts, the player's slot joins as a squad and gets HQ's order, the client mirrors the server's war (its
    status line matches the server's). **Not verified end to end:** the client's requests. Since d164ad3bd
    (2026-10-07) the developer harness rejects every command in multiplayer, so no Trident MP scenario can drive a
    client past its briefing; the stock `dedicated_init_order_probe` fails the same way here. The scenario needs
    that guard to have a test opt-in; `POSEIDON_CONQUEST_NETTEST=1` makes a client send a command and an order once
    it is in the game, for a by-hand check from the two logs.
- **Commander view, less clutter:** DEPLOY/SUPPORT opens with B or the top bar's button; compact squad rows with the
  task on the selected one; the controls row behind H; callsigns over the 3D squads only for the selected and
  hovered ones; one capture circle per objective. **Minimap** (World in Conflict style, bottom left): the island,
  objectives, your squads, known enemies, the shroud and the camera's footprint; click or drag to move the view,
  right click to send the selected squads.
- **Follow camera:** follows the unit where it is drawn (between ticks) and pins the camera's own interpolation; the
  followed truck's frame-to-frame jump on screen fell from a median 1.09 px (p90 3.35) to 0.22 px (p90 0.89).
  A one-frame lag remains (the camera is placed after the scene is drawn).
- **Also:** voice confirmations for the commander's orders (the leader's own radio voice), Ctrl + middle drag pans,
  a free troop transport at every captured base, and an **arsenal** (the owner's `ReammoBox*`) at every owned
  post, restocked every 10 minutes; the inventory's vicinity pane takes gear from it (in multiplayer the classic
  Gear screen, as the physical inventory is single-player only).

## Status, 2026-10-26 (evening): Slice 4 -- the LLM commander, HQ conversation, AI & voice settings

- **AI & voice panel** (the setup screen's and the commander view's "AI & voice..."): talking to HQ and the squad on
  or off; the language model (provider, model, address for Ollama/Custom, key, a Check button with the connection
  state); voice input (speech to text: provider, key, check; hold Caps Lock to speak); voice output (text to speech:
  provider, key, volume, radio voice, Test voice, check); "remember keys" (DPAPI-encrypted, outside the profile). It
  reads and writes NPC chat's own `npcChat.json` and key store, so it and the dev panel's tab are two views of one
  configuration, and it works without `--dev`.
- **LLM commander, Hybrid** (`Game/Conquest/ConquestLlm.*`), per side, chosen on the setup screen (or
  `<SIDE>commander = "llm"`). Every 4 game minutes, and 30 s after any change of hands, the side's report goes to
  the model: objectives with owner and the enemy the SIDE has seen nearby (its target database, never ground
  truth), its squads and tasks, supplies, prices, its own recent log lines. The model answers with priorities
  (-2..3 per objective), a posture (attack / balanced / defend) and maybe a purchase. The Standard commander folds
  the priorities into its target ranking, the posture into its force ratio and garrison size, and makes the
  purchase when it can; it keeps every assignment. A request runs on a background thread, one per side; an answer
  that comes after the front has changed is dropped; a failure (no key, transport error, prose, invalid JSON,
  invented towns) is logged and the Standard commander carries on.
- **Memory:** the commander's last 8 decisions (game minute, posture, main effort, purchase, reason) go into every
  later report as "Your earlier decisions", and are saved with the game.
- **HQ conversation:** NPC chat's situation report carries the war as the player's side knows it and HQ's plan
  (main effort, the commander's intent, the player's squad order), so HQ answers from the real state. "HQ, put
  everything on Figari" makes the model add a `priority` order; Conquest accepts it as the main effort for ten
  minutes, or refuses an objective that is not on the front yet, and HQ says which.
- **Checked:** `PoseidonTests "[conquest]"` (19 cases, 5 new: the answer parser and the priority order);
  `scripts/Test-ConquestLlm.ps1` against a local stand-in model (`scripts/conquest_mock_llm.py`, no key, no cost):
  the plan is applied (main effort, posture, purchase), the second request carries the memory, EAST's report holds
  none of WEST's orders, the player's request to HQ becomes the main effort; a prose answer and an unreachable
  server both leave the Standard commander running. Not checked against a real provider (the owner's keys and
  quota). Talking to HQ is single player, as NPC chat is; in multiplayer the LLM commander runs on the server.
- **Capture by majority** (owner, 2026-10-26): the side with more soldiers in a capture zone captures it; only a tie
  freezes the bar; the speed follows the margin (`CaptureMargin`: 8 against 3 is as fast as 5 alone).
- **Arsenal boxes, checked:** `conquestArsenals` applies the physical inventory's own test (a `VehicleSupply` in
  the landscape's object lists with cargo). The stock `ReammoBox*` crates carry magazines only (61), so a box is
  now also stocked with its side's weapons (`<SIDE>arsenalWeapons[]`; defaults: BLUFOR M16, M16 grenade launcher,
  M21, M60, LAW, Carl Gustav, binoculars; OPFOR AK-74, AK-47, SVD, PK, RPG, binoculars). Not opened by hand in
  the inventory screen yet.
- **Arsenal prices** (owner, 2026-10-26): taking from an arsenal box costs the box owner's supplies:
  `arsenalWeaponCost` 25 a weapon, `arsenalMagazineCost` 3 a magazine. The physical inventory checks before the take
  (unaffordable gear is not offered; a refusal shows the game's hint with the price) and charges once the item has
  moved (`Conquest::ArsenalTake`, called from `InventoryModel::CanTransfer` / `Transfer`). Checked through the
  inventory's own transfer (`conquestTestArsenalTake`): a magazine 400 -> 397, a weapon 397 -> 372, both refused at
  1 and 10 supplies. Multiplayer's classic Gear screen does not charge yet.

### Status 2026-10-26 (later): the war's settings, the Resistance, every village, air support, multiplayer command

- **Every village of Everon** is an objective now (18: the 7 before plus Regina, Durras, Vernon, Saint Pierre, Levie,
  Laruns, Chotain, Provins, Le Moule, Lamentin, Meaux), linked into one front graph. Le Moule is the Resistance's
  base. The objectives moved to each mission's `objectives.hpp`; `conquest.hpp` (the rules) is the same file in
  every Conquest mission, and `Build-Conquest.ps1` stops if a copy drifts.
- **The Resistance** (the game's third side, GUER; "Resistance" is its name in this game, flag `guba_vlajka.pac`) is
  optional and off by default (`resistance = 0`). Off, its base is one more neutral village (`DropLastSide`). On,
  it has its own rosters and vehicles (Ural, UAZ, BMP, Mi-17, T-55, Mi-24) and everyone is hostile to everyone.
- **Setup screen, THE WAR:** the Resistance on/off, villages to win (0 = off, up to every village of the island:
  `VictoryTownsMax`; `TownVictor` ends the war when a side holds that many), and each side's starting supplies
  (0..5000). Saved with the game. `conquestStart` takes them too: `[side, start, fog, view, llm, resistance,
  villages, [supplies]]`.
- **Each side starts with a tank and a helicopter** (startSquads adds Armour and AirAssault).
- **Commander personalities:** `<SIDE>personality` = balanced / aggressive / cautious sets the AI's starting posture
  (WEST balanced, EAST aggressive, GUER cautious); the LLM commander may change it.
- **The commanders use the artillery:** a Standard commander fires its mortars on a known cluster of enemies (3+,
  a vehicle counts 2) at an objective it attacks or holds, never within 120 m of its own men, only with a rifle
  squad's supplies to spare. The LLM commander can name an objective under `"barrage"`. Mortars now need an
  objective of the side within `barrageRange` (4 km) and warn friendly squads within 300 m ("danger close").
- **Air support** (owner, 2026-10-26): the SUPPORT panel's Air strike (and `conquestAirStrike [x, y]`): the side's
  attack plane (A-10 / Su-25) with a pilot comes in from the side's nearest objective, over the point, and drops a
  stick of 4 bombs 12 m apart along its track, aimed at the point (cost 400, 5 min rearming, one plane at a time).
  It can be shot down before the drop. The AI commanders call it on armour or crowds when the mortars cannot.
- **Plan leftovers done:** the end screen (VICTORY / DEFEAT, why, each side's holdings and losses; New war in single
  player, which clears the island and opens the setup screen again; Main menu; Look round), the war history
  (`conquest-history.txt` in the profile, one line per war), capture weights (a crewed armoured vehicle counts 3,
  anything more than 30 m up counts 0), the purchase queue (a purchase the side cannot make yet waits up to 2
  minutes and deploys when it can; HQ radios when it does or is cancelled).
- **One commander per side** (multiplayer): the first player to open the command view holds command; another gets
  "X commands WEST. Ctrl+V calls a vote to replace them." Ctrl+V calls a vote, Ctrl+Y / Ctrl+N vote (60 s; a
  majority of the side's players removes the commander, who fights on as a soldier and may not take command for
  5 minutes). The vote shows as a card in both views.
- **Human players in the command view** carry a white badge (head in a ring) over their symbol and "PLAYER" on the
  squad label; a squad a human leads is never moved by the commander: the order reaches it as HQ's request (order
  card and radio). A human player's soldier cannot be taken over.
- **The commander's mark:** a small gold star over the commander's soldier, for his side only, within 300 m, fading
  with distance (and in the command view). A different uniform is not possible: a live soldier's model cannot be
  swapped without replacing him.
- **Slice 5, other islands:** `objectives = "auto"` makes the war from the island's own town list (CfgWorlds Names,
  villages and up; `AutoObjectives`: HQs as far apart as possible, each town linked to its 3 nearest within 2.5 km
  plus a spanning tree so nothing is cut off). Missions for Malden (Abel), Kolgujev (Cain) and Nogova (Noe) are in
  the Single Missions list; the CONQUEST menu entry still opens Everon. The spectator body is moved off the
  battlefield (land, 600 m from every objective, near the player's HQ) before the setup screen.
- **Squads awaiting orders hold still** (owner: "do not let them run around like chickens"): while a human commands
  the side, the AI planner moves none of its squads; a squad without an order is told once to stand where it is,
  in formation round its leader; a bought squad waits where it deployed; the human's orders no longer run out
  after 10 minutes while he commands. The AI takes the squads back only when nobody commands the side.
- **Air support is a real plane:** the bombs are the plane's own shots, released from it over the target. Shot down
  (or its pilot killed) before the target, nothing drops and HQ radios "Air support lost".
- **The shroud is never the default** and is a top-down overlay only: it fades out as the command camera comes down
  and is not drawn near the ground (the minimap keeps it). The tests use the default fog of war; the shroud gets
  one capture from 4 km up.
- **Checked:** `PoseidonTests "[conquest]"` (23 cases); `scripts/Test-ConquestFeatures.ps1`, five runs, all checks
  passed: deaths carry the player on, the Resistance out by default and in on request, the command view,
  mortars, the air strike dropping on its point, a plane shot down dropping nothing, one plane at a time,
  Malden's war made from its 14 towns (HQs Goisse, Le Port, and La Riviere for the Resistance). Not checked: the vote and the one-commander rule in a real multiplayer game (the harness
  refuses commands in multiplayer), the end screen's New war by hand.

### Status 2026-10-26 (evening): options, multiplayer arsenal, islands in multiplayer, squad commands, stuck squads

- **Setup options:** a time limit (0 = off, up to 4 hours; then the higher war score wins, the top bar counts down)
  and "the Resistance fights with WEST" (with the Resistance on). Allies (`War::allied`) never attack each other's
  objectives, an ally's men count as the owner's in its zones (`MergeAllies`), an ally near a base does not block
  deploying there, and the war ends when only allies are left. `setFriend` makes the AI agree.
- **Arsenal charging in multiplayer:** the classic Gear screen's takes (VehicleSupply in `Transport.cpp`: take
  weapon, take magazine, rearm) check and charge an arsenal box like the physical inventory does; the clients
  learn which boxes are arsenals from `conquestNet`, and a client's charge goes to the server ("arsenal" request).
  This also covers single player's action-menu takes, which were free before.
- **Multiplayer missions for Malden, Kolgujev and Nogova** (`mpmissions/conquest-*-mp.*`): eight player slots a
  side and the respawn markers at the HQs the generator picks for each island (Malden: Goisse / Le Port; Nogova:
  Mokra vrata / Okrouhlo; Kolgujev: the grid squares 043-028 / 118-105).
- **Kolgujev has no town names** in the game's config (CfgWorlds >> Cain >> Names is empty), so its war could not
  start. Its objectives.hpp now lists the island's 21 settlements, found from the world file's buildings
  (clusters of five or more within 250 m), named by map grid; `CfgConquest >> Places` feeds the generator.
- **The soldier cap is 80 a side** (160 alive measured at about 1 ms of AI a tick); 55 a side with three sides.
  Each side now starts with two tanks.
- **Situation reports on the radio:** the LLM commander's answer carries a `"radio"` report HQ reads out; sides on
  the standard commander get HQ's own report every 5 minutes (towns held, the fighting, the threatened town).
- **AI & voice panel:** a quick setup with one Google AI Studio key for the model, voice input and voices; the
  model picked from the provider's own list, reset to the provider's default on a change of provider; a change of
  voice provider resets the voices to its male pool; the panel says why Windows voices speak (no key, Google Cloud
  picked instead of Gemini, a refusal). Hold-to-talk moved from Caps Lock (the game's voice-over-net key) to U.
- **The owner's LLM never answered:** `gemini-3.8-live` is a realtime voice model ("only supports real-time
  bidirectional streaming via WebSocket"): 16 of 16 requests refused with HTTP 400. Live, native-audio and
  realtime models are no longer offered for chat, and the commander falls back to the provider's default model
  (once more on a refusal of the model itself), saying so in the war log. A voice pool picked for another
  provider (the owner's held ElevenLabs voice ids under Gemini) is replaced by Gemini's male voices.
- **Command view:** C (or the bar's Me) centres on the player's own soldier and follows him; zoom out to 9 km; the
  selected squads' routes are drawn in steps (shown even with one end off screen) and on the minimap, and squads
  without a move order show the task's objective; contextual commands for selected squads: Engage, Hold fire,
  Stop, Board, Get out / Land and unload, Land, To base (`conquestSquad` from a script).
- **Helicopters no longer hover over the drop:** the dismount ordered the pilot out with the passengers; a pilot
  cannot leave a flying helicopter, so it hovered for ever. Now the helicopter is told to land for its passengers
  and its pilot stays at the controls.
- **Squads standing about:** every captured town kept its capturing squad as a garrison until threatened, and a
  rear town never is; with 18 villages most of an army ended up guarding its own back yard. Rear towns (every
  neighbour the side's own) keep no garrison now. Stuck squads (no progress for 90 s and not fighting) are nudged:
  a truck or APC squad gets out and walks, a squad on foot is ordered again by a point to the side.
- **Supply lines** (owner, 2026-10-26): the HQ spends the side's pool (the towns' income goes there); every
  other base keeps its own stock, spent by deploying there and by its arsenal. A base's stock barely grows by
  itself (2 a minute) and is lost when the base changes hands (`StepCapture`). Supply trucks (`Supply`: a crew
  in a Truck5tReammo / UralReammo, deployed at the HQ only) load up to 300 at the HQ, drive to the base that
  needs them most (little stock, on the front, not already served), unload and come back; a destroyed truck
  loses its load. The AI keeps one truck for every three bases (at most three). The commander can click a
  base's command tent for its card (supplies, Deploy here, Send a supply truck), right-click a base with a
  supply truck selected, or put any squad with a truck of its own on "Supply runs" (and back with "Fight").
  `ConquestRefereeSupply.inc`.
- **No idle squads** (owner): a squad without a task always gets the AI's (attack, take, secure); while a human
  commands, the AI only fills in and never re-tasks a busy squad; a bought squad waits 2 minutes for orders.
- **Support waits 5 minutes** (`supportDelay = 300`): no mortars or air strikes, the AI's included, in the
  war's first minutes, so nobody shells a freshly deployed base.
- **Engage a target:** the Engage button, then a click on a building or vehicle (within 30 m; not the war's
  own posts, flags and arsenals): the selected squads attack it (`AIGroup::Attack`), a gunship flies in on it.
- **Helicopters** unload with only the pilot aboard and then set down and stay (no more trailing the squad at
  40 m); checked: the Mi-17 at Tyrone on the ground with its pilot alone.
- **Vehicles park apart:** each base hands out parking slots behind it, 28 m apart (45 m for helicopters), so
  the starting trucks, tanks and helicopter no longer appear on one spot.
- **Command view:** enemy markers are a setup option (off by default; set before the war); the mouse wheel
  zooms on a selected soldier or squad, else where the cursor points.
- **Soak 2026-10-26 (40 game minutes, 4x):** 25 captures and 2 neutralisations, the supply line ran (6 runs,
  4 delivered); 37 of 42 stuck-nudges were one tank and one helicopter squad already at their town, so a nudge
  now needs the squad outside the town and no helicopter landing. WEST barely bought: the soak opened the
  command view at the start, which handed WEST to "the player"; the soak now starts with `noview`.
- **What a squad is doing, in words:** the squad list and the label over a selected squad say what it is doing
  there, from the referee (`Doing`): "Supply run to Figari (300)", "Back to the HQ for supplies", "Taking
  Tyrone", "Attacking Gravette", "Securing Meaux", "Engaging Zelt", "Holding position, as ordered", "Awaiting
  orders", "(landing to unload)"; clients get the server's words with each squad.
- **File split:** the referee's air support, supply line, contextual orders and multiplayer command moved to
  `ConquestRefereeAir.inc`, `ConquestRefereeSupply.inc`, `ConquestRefereeOrders.inc` and
  `ConquestRefereeCommand.inc` (the FileSize gate stops at 5000 lines).

### Status 2026-10-26 (night): the audit's leftovers

An audit of every owner request and every plan item (the checklist is in the side-session notes) left these,
now done:

- **The CONQUEST menu chooses the island** (Everon, Malden, Kolgujev, Nogova): `UI/ConquestIslandDisplay.cpp`,
  started from `DisplayMain::OnChildDestroyed`.
- **AI soldiers' skill** (setup: recruits / regulars / veterans, `aiSkill`): `setSkill` 0.35 / 0.6 / 0.85 on
  every soldier the war makes.
- **A different LLM model per side** (setup, `<SIDE>llmModel`).
- **The last war's settings are kept** in the profile (`conquest-settings.json`): the setup screen opens on
  them and New war starts with them (3.7, 4.6). Single player only.
- **Tanks and aircraft deploy only at the HQ or an airfield** (3.4); the AI buys them there.
- **Mortar accuracy depends on an observer** (3.8): 40 m with a soldier of the side within 1 km of the
  target, 120 m without (a distance test, not a true line of sight).
- **Asking HQ for fire support** (3.8): in the HQ conversation, "mortars on Figari" or an air strike becomes
  a `barrage` / `air_strike` order; HQ fires or says why not.
- **A Trident integration test** (7): `tests/integration/conquest/conquest_war` boots the Everon war with both
  sides on the AI and checks it through `conquestStats`. Its strict mode found a real bug at once: WEST's
  helicopter was parked in the surf behind Morton, counted as unusable, and its squad could never board
  (`AISubgroupFSM` check failures). Parking slots now need dry land 3 m above the sea.

Deliberately different from the plan: the death handling (the owner asked that the player carries on in
another soldier rather than a paid redeploy screen); artillery flight time and cooldown (7 s and 2 minutes as
built and played, plus the 5-minute start delay). Not done: virtual far groups (not needed at a cap of 80);
imported worlds (each needs a mission and testing on that world's data); a radio-menu (0-8) entry for taking
command (Ctrl+M does it).

### Status 2026-10-26 (late): stuck squads, the arsenal's ammunition, shorter tests

Stuck squads, from four soaks (80 game minutes each, both sides on the AI):

- **A truck squad that got out went straight back in.** The engine's own move and attack logic re-boards a
  squad on any move over 200-300 m, so a squad that dismounted from a stuck truck was in the same truck a
  minute later (one squad: 20 times in a war). A stuck vehicle now first backs off 30-75 m to one side and
  tries again (`Detour`, `ConquestRefereeMove.inc`); a truck squad still stuck after that leaves its trucks for
  good (`AIGroup::UnassignVehicle`, the script command `leaveVehicle`) and walks.
- **Supply trucks had no stuck handling at all.** They back off too; one that five detours do not free is left
  behind, its load lost. A supply crew without a truck (destroyed or left) fights on foot instead of standing
  at the HQ for the rest of the war, and the commander buys a new truck.
- **The same order every plan.** A worn-down squad in a rear town was freed and given the same "regrouping"
  task back on every plan (39-145 times a war); each re-order planned its path anew and reset its progress, so
  it was never seen to be stuck. The referee now ignores an assignment a squad already has (unless it stalled).
- **The engine's get-in check** (`AISubgroupFSM` "veh == unit->VehicleAssigned()"): a get-in command whose
  vehicle is no longer the unit's (given up, unassigned as unable to move, or a seat change) tripped the check
  on entering a state and sent the soldier toward a vehicle that was not his. The unit now stays put and the
  next check ends the command.

| Soak (80 game min) | captures | truck squads sent on foot | stalls | engine errors |
|---|---|---|---|---|
| before (3b2e1be40) | 61 | 31 (one squad 20x) | 11 | 0 |
| after | 86 | 1 | 4 | 0 |

**The arsenal's ammunition**: the side's ammo box holds only the common magazines, so the sniper rifle (and the
Carl Gustav) came with nothing to fire. Every weapon the arsenal offers now brings five magazines a weapon,
read from its config (each muzzle; "this" is the weapon's own name).

**Shorter tests**: the soak runs to `-GameMinutes` (80) at `setAccTime 8` with the frame rate uncapped (the
old 60 fps cap held it to 4x: the engine runs at most 8 fixed steps a frame) -- 12 real minutes instead of 20.
The feature test runs its waits for planes and the helicopter at 4x: 4 minutes instead of 6.

Open: an occasional "Out of map" (OperMap path planning falling back to a straight path, 0-3 a war) and the
pre-existing `[ai]` unit test "EntityEvent enum names" failure; neither is from Conquest.

### Status 2026-10-26 (night, 2): the owner's play-test round

- **Wheel zoom** moved the camera: zooming out slid it away from the pointer, a pointer near the horizon
  (ground kilometres off) moved it far per step, and a selection long scrolled out of view pulled it there.
  Now only zooming in moves the camera, by at most a third of its distance a step, toward the selection only
  while it is on screen.
- **Orders are answered on the radio** in the squad leader's own voice when NPC chat's voices are on
  (`NpcChat::SayOnRadio`, a voice per squad, through the radio filter): "Bravo copies, attacking Figari".
  Without voices, the game's own radio confirmation as before.
- **Any soldier**: the selection bar lists the selected squad's soldiers (leader first); a button or Tab
  focuses one, the camera follows him, and Take control (T) becomes exactly him. A vehicle picked on the
  map stands for its crew: the squad's man in it is the soldier picked. Players are never taken over.
- **The AI takes command at once** when the player leaves it (the command view closed, a soldier taken
  over): orders the human gave were held for 10 minutes and the planner waited for its round; now they are
  released and the side replans the same tick. Supply runs the commander set stay.
- **Supply trucks under a human commander**: the supply line kept its trucks only under the AI, so a side
  the player commanded had none. Every side now keeps one per three bases (at most three).
- **More supplies at the start**: 2000 a side (was 500; the setup screen sets each side's, 0-5000). A profile
  still holding the old default takes the new one.
- **Fire support**: the AI called its mortars and plane almost never (none in an 80-minute soak): it decided
  after spending on squads and kept a squad's price in reserve, wanted three enemies within 60 m, and
  refused with any of its own men within 120 m. Now support comes before purchases, two enemies (a vehicle
  counts twice) within 80 m are enough, and the safety distance is 90 m.
- **Helicopters never hold a place**: a gunship's crew of two counted as a worn-out squad, which only ever
  defends -- gunships "regrouped" at bases. Vehicle crews are never worn out now, and a helicopter squad still
  in the air (a gunship, or air assault not yet set down) only attacks or carries.

Tests: `mouse_wheel` harness command and `conquestCameraState`; the feature test checks the wheel, Tab and
T on a non-leader, the radio answer (offline Windows voices) and the AI taking over.

Later the same night (the owner asleep, soaks only):

- **Fire support, the real cause**: a contact's height is above sea level, and the AI skipped "aircraft" over
  15 m -- on Everon nearly every enemy. Now height above the ground: 7-8 barrages in a war (none before).
- **Air assault helicopters go home**: on an AI side, once its passengers are out, the pilot leaves the squad
  with the helicopter, flies it back to the HQ, lands, and is stood down (30% of the squad's price back). No
  helicopter waits by a town, and the squad goes on under its next leader (the pilot often led it). Under a
  human commander it still sets down by the squad, for the commander's Board order.
- **AI purchases spaced**: one squad a base every 30 game seconds. Rich at the start (2000), the AI bought
  a dozen at once and they jammed the roads out of their HQ.
- **"Out of map"** (OperMap path planning falling back to a straight path) now names the unit and its fields,
  to find what plans from outside the map (27 in one second once, after a supply truck backed off).
