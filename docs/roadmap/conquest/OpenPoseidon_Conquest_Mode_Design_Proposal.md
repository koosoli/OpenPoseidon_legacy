# OpenPoseidon — Conquest Mode
## An Open Design Proposal and Collaborative Development Checklist

**Status:** Concept proposal / discussion draft  
**Initial target:** Original *Operation Flashpoint / Arma: Cold War Assault* Everon island  
**Primary experience:** Single-player, NPC-driven, persistent-feeling territorial warfare  
**Design philosophy:** Start with a small playable scenario, reuse existing engine systems where practical, and leave technical and creative decisions to the implementing developer or AI agent.

> **To the AI or developer exploring this idea:** This is a description of the experience we would love to create, **not a set of implementation commands or a prescribed architecture**. Please inspect the current OpenPoseidon codebase, challenge any assumptions here, propose better solutions, and add your own ideas freely. You may reorganize priorities, substitute more suitable mechanics, identify hidden dependencies, or recommend a completely different technical approach if it improves the game. The checklists are prompts for discussion and progress tracking, not an obligation to implement every item verbatim. We are particularly interested in your independent judgment about what is already supported by the engine, what should be reused, and what would be fun to play.

---

## 1. The vision

Imagine starting OpenPoseidon, choosing **Conquest** from the main menu, selecting one of the familiar Cold War factions, and entering an island-wide conflict that unfolds without scripted missions or human opponents. Friendly and enemy soldiers, squads, vehicles, and commanders are NPCs. The factions fight for territory, earn resources from their holdings, purchase reinforcements, capture bases, defend their territory, and decide where to attack next.

The player is a participant in a much larger, autonomous conflict. They can fight as an ordinary soldier, talk to friendly squadmates and headquarters, request support, or take over the **Commander** position and direct the war from a map-based strategic interface. The war should continue to function when the player takes no action, even if it remains a conventional single-player simulation that runs only while the game is open.

The intended feel is inspired in part by *Arma Reforger's* territory-control / Conflict-style gameplay, combined with classic *Operation Flashpoint* factions and atmosphere, and an optional Command & Conquer-like perspective for commanding NPC forces. This is inspiration rather than a demand for exact mechanical imitation.

### High-level player experience

- The player selects a faction at the beginning of a session or campaign.
- The first playable map is **classic Everon**, using its original game assets and geography.
- The factions are **US forces, Soviet forces, and Resistance / rebels**. A two-faction prototype may be a sensible first step, with the third faction added once the systems work.
- All sides have an autonomous AI commander, unless a human player assumes command of their own faction.
- Bases and strategic locations can change hands through active combat and occupation.
- Controlled bases generate resources over time, making logistics and territorial expansion meaningful.
- Units and vehicles cost resources and are spawned or deployed only from eligible, safe locations.
- The player's actions can affect the overall war, but the war does not revolve exclusively around them.
- The experience can remain open-ended and replayable, with a clear victory condition for a given war and the option to begin another.

### Core fantasy

**“I am one soldier in a living war, but I can also become the commander and shape that war.”**

---

## 2. Suggested game flow

One possible flow might look like this:

1. **Main menu:** A new *Conquest* entry opens a dedicated game-mode screen.
2. **Setup:** Choose Everon, one of the available factions, and perhaps simple scenario options such as AI difficulty, number of bases, starting resources, and optional LLM commander behavior.
3. **Deployment:** Spawn at a valid faction-controlled headquarters or base, initially as a regular soldier.
4. **Living front line:** AI commanders make plans, send groups between objectives, react to threats, reinforce positions, and invest resources.
5. **Player agency:** Fight in a squad, request artillery through HQ, communicate with friendly NPCs, join a defense, or apply to become Commander.
6. **Commander application:** If the player's faction has no human Commander, they may assume the role; the AI Commander hands over control gracefully. The player may later step down, returning command to AI.
7. **Victory / replay:** A faction eventually wins under agreed conditions; the player can start another war, potentially with different settings or a different faction.

The exact menu flow, application mechanism, victory criteria, and respawn rules are **open design questions**. The above is a starting point.

---

## 3. The three factions

The desired factions echo classic *Operation Flashpoint*:

| Faction | Conceptual role | Potential distinguishing flavor |
|---|---|---|
| **US / WEST** | Conventional NATO-style force | Western equipment, combined-arms operations |
| **Soviet / EAST** | Conventional opposing army | Soviet equipment, armor and infantry operations |
| **Resistance / GUER** | Local rebels / resistance | Guerrilla tactics, lighter logistics, different starting positions |

Ideally, all three could compete on the same map. That creates interesting strategic dynamics but also requires explicit rules for diplomacy, hostility, and control of territory. Initial assumptions might be that all three compete independently, but it would be worth discussing whether temporary alliances, asymmetrical starting resources, or configurable faction relationships improve the experience.

Possible questions:

- Should the third faction be available from the first prototype, or only after two-faction gameplay is proven?
- Should all factions obey exactly the same economic rules but have different equipment, or should Resistance have its own asymmetrical economy?
- Is each side always hostile to the other two, or could relationships change?
- Can a faction recover from near-defeat without generating endless stalemates?

---

## 4. Capturing and holding bases

**Proposed basic rule:** Strategic locations have a faction owner and a capture area. Friendly forces must establish control of the area for a period of time. Enemy presence contests or prevents capture. Capturing a location changes ownership, may unlock deployment options, and provides a regular resource income.

This is inspired by *Arma Reforger* and could use a radio tower, command post, flag, or other identifiable capture object—but the actual location-based mechanic should be selected for clarity, robustness, and playability. A visible capture bar and explicit *Contested* feedback would help players understand events.

Potential considerations:

- Is capturing based on one soldier's presence, minimum troop strength, or a numerical advantage?
- Do enemy NPCs contest the point, pause progress, or reverse it?
- Should the capture process have a neutralization phase before switching ownership?
- What happens when troops leave midway through a capture?
- Can several squads capture faster, and if so how do we prevent exploits?
- Are all control points equally important, or are HQs and logistical bases special?
- Could roads, radio towers, or connected territories influence reinforcement access later?

**Tentative initial preference:** Straightforward area control with visible progress and contested-state handling, before introducing an elaborate communications or supply-network requirement.

---

## 5. Bases, resources, purchasing, and spawning

A compact economy should make strategic decisions matter without turning the game into a complex spreadsheet.

### Possible single-resource model

- Each controlled base produces a certain amount of a shared resource at regular intervals (for example, once per minute; the rate remains a balance decision).
- Headquarters and perhaps certain special bases provide deployment / production facilities.
- Infantry, squads, vehicles, and support actions such as artillery consume resources.
- Capturing more bases increases the faction's income and its capacity to reinforce or conduct operations.
- AI commanders and human commanders draw from **the same actual resource pool** and respect the same purchase and deployment restrictions.

### An important rule already agreed in discussion

**No spawning at a base that is actively under attack or contested.** New soldiers or vehicles should not materialize beside hostile forces during a battle. Recruitment and deployment should take place at faction-owned, eligible, sufficiently safe locations. If a location is contested, a reinforcement order could wait, relocate to another valid base, or be rejected with a clear explanation.

The precise meaning of *under attack* needs a robust, non-exploitable definition. For example, an enemy presence within a security perimeter or a recent confirmed hostile action might disable spawning temporarily, with a sensible grace period before re-enabling it. The precise implementation and balance are open.

Other considerations:

- Should purchased soldiers appear as individual units, ready-made squads, or reinforcement waves?
- Must vehicles spawn at appropriate depots and aircraft at airfields?
- Can distant bases receive physical supply convoys in a later phase?
- Can a faction become permanently unable to respawn if its economy collapses? Should a limited emergency recovery mechanism exist?
- Should the player's own respawn cost resources, and what is the fallback when none are available?
- Do deployed units have upkeep, ammunition costs, or only a one-time purchase price?
- How are AI unit caps and performance budgets enforced without making the economy feel artificial?

**Initial preference:** One simple resource type, safe-base deployment, clear unit costs, and no complex production chains until the game loop is enjoyable.

---

## 6. Two ways to play: Soldier and Commander

### Soldier experience

The default player role is an ordinary soldier belonging to the chosen faction. The player can:

- Move, fight, drive, and cooperate with friendly AI as in normal OpenPoseidon gameplay.
- Receive assignments and situation reports from the AI Commander.
- Request reinforcements or artillery, potentially through the existing NPC chat / radio system.
- Ask HQ about the strategic situation.
- Apply for the Commander role without having to choose it permanently before the game starts.

### Taking command

A proposed approach is a **Commander application / takeover** option accessible during play. If the role is available, the player becomes the faction's Commander. An AI Commander handles the faction when a human is not in that role.

While Commander, the player could still use their soldier avatar and return to first-person combat. This provides the option to lead from the battlefield rather than being locked into an RTS-only game.

### Strategic command interface

The Commander can switch to a top-down map or tactical overview, inspired by RTS games such as *Command & Conquer*. Potential interactions:

- Select one or multiple squads or vehicles, including drag-selection / lasso where appropriate.
- Issue move, attack, defend, patrol, hold, regroup, and reinforce commands.
- Select bases and inspect income, deployment eligibility, and production status.
- Purchase units at valid locations.
- Mark a point on the map and request artillery support.
- See own units and only legitimately known enemy contacts (fog of war).
- Return to normal soldier gameplay while issued orders continue.

There is already a map, unit-command systems, and development tooling in OpenPoseidon; reusing these may be more sensible than creating a completely separate real-time strategy engine.

Potential details worth exploring include whether the command view is a 2D map first, a 3D overhead camera later, or a hybrid. Selection should be accessible without losing the feel of an infantry game.

---

## 7. AI commanders and autonomous war

Every faction needs a commander capable of fighting without direct player intervention. The war should remain interesting even if the player simply watches.

A conventional AI Commander might periodically:

- Evaluate the strategic importance and vulnerability of bases.
- Estimate friendly strength and known enemy presence.
- Decide whether to defend, reinforce, probe, assault, withdraw, or build reserves.
- Buy units and vehicles from its actual resource pool.
- Assign squads to objectives and update plans as the situation changes.
- Handle unit losses, stalled attacks, contested deployments, and unavailable resources.

This is fundamentally different from individual squad combat AI. The strategic Commander decides **what should happen and why**; existing tactical AI decides **how soldiers carry out those orders locally**.

An important design possibility is a **hierarchical simulation**: distant, unobserved forces might resolve movement and engagements at a cheaper strategic level, while units near the player use the existing full simulation and physics. The transitions must be credible, deterministic where required, and unable to create impossible outcomes. This is a likely major technical challenge rather than an already solved feature.

The commander should not secretly spawn free soldiers, know undiscovered enemy locations, or bypass the rules. Fairness and consistent state are central to the experience.

---

## 8. Optional LLM Commander — a planned capability, not a dependency

A particularly interesting extension would allow the user to choose between **Standard AI**, **LLM Commander**, and potentially **Hybrid Commander**. The engine already supports multiple local or hosted language-model providers through its experimental NPC chat feature. The Conquest system could build on that infrastructure without requiring an LLM to play the mode.

### Possible operating modes

| Mode | Strategic decisions | Tactical execution |
|---|---|---|
| **Standard** | Deterministic / conventional game AI | Existing game AI |
| **LLM** | Language model chooses high-level plans from structured information | Existing game AI |
| **Hybrid** | LLM proposes or revises plans; conventional planner validates / supplements | Existing game AI |

A key idea is to make these choices **interchangeable behind the same Commander interface**, so they produce the same kinds of high-level actions and follow the same gameplay rules. The developer is free to propose a better separation if warranted.

### Information an LLM might receive

A compact, structured **faction-specific situation report** could contain:

- Known bases, ownership, importance, capture progress, and attack status.
- Friendly units, approximate strength, condition, position, current orders, and availability.
- Enemy units **only where actually detected**, with last-seen timestamps and uncertainty.
- Resource balance, income, potential purchases, and deployment restrictions.
- Known routes, objective distances, available artillery, and relevant support constraints.
- Current strategy, active orders, recent outcomes, urgent alerts, and player requests.
- The list of legal high-level commands and any necessary command schemas.

**Fog of war must apply to AI commanders too.** An LLM should never receive omniscient world state simply because the engine knows it internally.

### Decisions and execution

The model might issue structured intents such as:

- Defend a threatened base with a particular group.
- Assault a neighboring objective using available forces.
- Buy an infantry squad at an eligible base.
- Reserve resources for a tank instead of spending immediately.
- Request or authorize artillery fire on a known coordinate.

The engine would still check authority, current ownership, available resources, spawn restrictions, range, cooldown, current units, and other game rules. **Model output should be treated as a proposal, not executable code or untrusted unrestricted commands.** A failed action should produce useful feedback so the next decision can improve.

### Frequency, reliability, and cost

LLM calls are better suited to occasional strategic decisions, important events, and conversations than frame-by-frame soldier control. For example, periodic planning every tens of seconds—with urgent reevaluation when a base falls—could be explored, while local movement remains entirely native. Exact cadence, token budgets, timeout behavior, batching, and model capabilities should be evaluated rather than fixed prematurely.

The game should run fully offline with Standard AI. When an LLM is unavailable, slow, misconfigured, or returns invalid output, the faction should continue operating through a reliable conventional fallback. Local inference should remain possible where the existing provider integration supports it.

### Natural-language interaction

The most appealing benefit of an LLM Commander may be that a soldier can have a meaningful conversation with HQ:

> **Player:** “Why aren't we attacking Montignac?”  
> **Commander:** “Our southern approach is exposed. I'm waiting for the reserve squad before advancing.”  
> **Player:** “Could we defend the crossroads first?”

Ideally, those answers reflect the Commander's **actual state and plans**, and a request can influence legitimate high-level decisions. The model must not fabricate successful orders that the game has not accepted.

### Multiple AI models

An interesting optional experiment is to allow different factions to use different models or strategies—for example, one conventional commander and two separately configured LLM commanders. That could become a fascinating AI-versus-AI sandbox, but it is **not necessary for the first playable version**.

---

## 9. Friendly NPC chat and user-facing configuration

Conquest would be a natural showcase for OpenPoseidon's existing experimental NPC chat system. The goal is to make it accessible to ordinary players, not only through developer tools.

Possible improvements:

- An understandable **NPC Chat / AI** section in normal game settings.
- Provider selection, model selection, connectivity / local model information, and essential options.
- Optional voice input and spoken responses where supported.
- Friendly NPC and HQ conversations through appropriate in-game interactions or radio channels.
- Enemy NPCs should not accept a player's military orders merely because they are chat-enabled; hostile interaction can follow different conversational rules.
- Clear messaging for unavailable models, API errors, rate limits, or disabled chat.

**Important consistency requirement:** The normal settings screen and Dev Tools should use the **same underlying configuration**. Changing a shared setting in either location should be reflected in the other; there should not be two drifting, contradictory configurations. Advanced diagnostics can remain in Dev Tools, while common settings are presented simply to players. API credentials should not be exposed in logs or situation reports.

Existing chat may be reused for ordinary friendly NPC dialogue, commander requests, radio orders, and strategic situation explanations. Whether these use one conversation system or several related modules is a design choice.

---

## 10. Artillery and paid support actions

Artillery is a particularly suitable way to connect the soldier and commander experiences.

### Suggested shared mechanic

- An artillery barrage has a resource cost, plus whatever other limits make sense (availability, range, cooldown, ammunition, communications, targeting information).
- **As a soldier:** The player asks HQ for support by radio / chat, giving coordinates or a map marker. An AI Commander reviews and authorizes the request when eligible.
- **As a human Commander:** The player selects artillery support directly on the tactical map and clicks a target area.
- **For AI commanders:** They can purchase or request artillery under the same rules rather than receiving free scripted strikes.
- All paths converge on one shared support-action system, so costs, authorization, cooldowns, visibility, effects, and feedback remain consistent.

Artillery could initially be an abstract support ability rather than requiring fully simulated physical gun batteries. A later iteration might require a real artillery asset, ammunition supply, firing range, and counter-battery vulnerability if these improve gameplay.

Questions include how to communicate danger to nearby friendly troops, how accurate unobserved fire should be, whether smoke missions should be included, and what penalties or limitations prevent constant bombardment.

---

## 11. Fog of war, logistics, and fair play

A strategy layer is more interesting when information and resources are imperfect.

- Friendly units and owned installations are generally visible to their own Commander.
- Enemy forces appear when spotted, reported, or recently observed, potentially with uncertainty.
- The AI and player should receive consistent visibility and intelligence rules.
- Strategic decisions should not exploit hidden information unavailable to that faction.
- Spawning and reinforcement should be tied to controlled and safe infrastructure.
- Routes, vehicles, roads, destroyed crossings, and the possibility of blocked movement could become strategically significant.

More advanced supply lines and communications networks would be interesting future additions. They are optional; requiring a complex network at the outset might delay a playable game unnecessarily.

---

## 12. Persistence, victory, and replayability

The initial idea is an **endlessly replayable** game mode, not necessarily a battle with no end at all.

One possible model is that a war continues until one faction defeats the others by taking the relevant HQs or strategic territories. The result is recorded, and another campaign can then begin with a new seed, faction, or settings. A configurable endless / comeback mode could be explored later.

A saved session would ideally preserve:

- Base ownership, contested state, and capture progress.
- The faction economy and available support actions.
- Surviving squads, vehicles, player role, and relevant unit state.
- Active AI orders, known enemy contacts, strategic plans, and mission events.
- The game clock, scenario seed, and other inputs needed for reasonable continuity.
- Where feasible, relevant world changes such as destroyed obstacles, wrecks, or terrain damage.

Persistent environmental damage is attractive but may need a separate technical evaluation because existing save-game support does not automatically mean every new deformation, effect, and imported-world state is serializable. The project can scope this incrementally.

Avoid a victory system that leaves one side stuck indefinitely with no usable spawn location or no chance of ending the round.

---

## 13. Scope: Everon first, other worlds later

The first target is **the original Everon map**, not a promise that every imported terrain and the Earth globe will immediately support Conquest.

If the mode proves enjoyable and robust, later candidates could include Malden, Kolgujev, Nogova, Arma-era maps, Reforger worlds, and potentially whole-world regions. Each world may require its own validated navigation, roads, locations, safe spawn placement, and strategic objective data.

Auto-generating bases and objectives from terrain and settlement data is an appealing long-term direction; hand-placing six good bases on Everon could be substantially more practical for the first test.

The engine already contains many relevant features, but the developer should verify which systems currently function together on the **actual integration branch**, rather than assuming all README features are production-ready or fully compatible with one another.

---

## 14. Possible implementation slices — flexible, not prescriptive

These are **suggested playable milestones**, not a mandatory development order. An implementing AI is welcome to reorganize them completely after code inspection.

### A. Confirm the technical foundation

- [ ] Identify the actual current branch / integration baseline and existing game-mode entry points.
- [ ] Review map, UI, scripting, squad-command, AI, economy-like, save/load, and NPC-chat systems for reuse.
- [ ] Assess actual Everon navigation, pathfinding, unit deployment, and world-state ownership.
- [ ] Identify performance constraints and likely save/load or networking pitfalls.
- [ ] Suggest a minimal implementation shape that fits the engine instead of adding redundant frameworks.

### B. First fun Conquest scenario

- [ ] A visible Conquest menu entry and an Everon launch flow.
- [ ] At least two opposing NPC-controlled factions.
- [ ] A small set of meaningful control points (perhaps around five or six).
- [ ] Bases with clearly displayed ownership and capture / contested feedback.
- [ ] A basic resource income tick linked to owned bases.
- [ ] Infantry deployment that costs resources and obeys safe-base restrictions.
- [ ] Two autonomous commanders capable of producing an observable, ongoing conflict.
- [ ] A player who can fight as a normal soldier and affect the war.
- [ ] A simple end condition, restart option, and enough debugging visibility to diagnose stalemates.

### C. Commander gameplay

- [ ] The player can apply for, take over, and relinquish their faction's Commander role.
- [ ] AI-to-player and player-to-AI handovers preserve current orders, resources, and ownership.
- [ ] A strategic map shows owned bases, known contacts, and friendly units.
- [ ] Multi-unit selection and high-level orders are usable without awkward micromanagement.
- [ ] Purchasing and valid-base deployment work for the human Commander.
- [ ] The player can return to soldier gameplay while orders continue.

### D. More complete war systems

- [ ] US, Soviet, and Resistance are supported as playable faction choices.
- [ ] Three-way victory / elimination, relations, and territory logic behave sensibly.
- [ ] Unit and vehicle purchases are balanced around resource income.
- [ ] Spawn denial works at attacked / contested bases, with clear feedback.
- [ ] Artillery is available through one shared costed support mechanic.
- [ ] Soldier radio requests and Commander map requests reach the same artillery system.
- [ ] Respawn, defeat, recovery, and restart rules avoid frustrating deadlocks.
- [ ] Save / load recovers a campaign reliably.

### E. Optional language-model integration and polish

- [ ] A common strategic Commander interface can support conventional AI and an LLM backend.
- [ ] Structured situation reports respect each faction's fog of war.
- [ ] LLM suggestions are validated against game rules before execution.
- [ ] Timeouts, errors, and offline use fall back to conventional AI without blocking the war.
- [ ] NPC Chat has clear player-facing settings linked to the same state as Dev Tools.
- [ ] Player-to-Commander and friendly NPC dialogue work in normal gameplay.
- [ ] Hybrid Commander behavior is evaluated for usefulness and cost.
- [ ] Optional different-model-per-faction experiments are considered, not assumed necessary.

### F. Quality, scale, and game feel

- [ ] Test a whole round with the player participating in combat.
- [ ] Test a whole round with the player doing nothing; the war should still progress.
- [ ] Check that commanders obey exactly the same economy and spawn restrictions as the human Commander.
- [ ] Check contested-base scenarios, unit shortages, route failures, player death, and last-base situations.
- [ ] Compare AI behavior and frame/simulation times at light and heavy unit counts.
- [ ] Verify that the Commander interface shows actionable, trustworthy information.
- [ ] Test save / resume during attacks and commander handover.
- [ ] Run compatibility checks so existing OFP missions, menus, AI, and save files are not unintentionally broken.
- [ ] Document the real limitations and accepted behavior, not only feature presence.

---

## 15. Open decisions worth exploring

These are opportunities for the implementing AI to propose better game design rather than asking the owner to settle everything up front.

| Topic | Tentative direction | Room for improvement |
|---|---|---|
| First version | Two factions, few Everon bases | Could three factions be feasible immediately? |
| Capture rules | Area control, timed bar, contesting pauses capture | Alternative objective or radio-tower mechanics |
| Economy | One resource generated periodically by owned bases | Initial balancing, costs, possible upkeep |
| Spawn constraints | Friendly safe bases only; contested/attacked bases disabled | Precise threat detection and fallback logistics |
| Commander selection | Start as soldier; apply to take over human Commander role | UI location, handover, voluntary retirement |
| RTS controls | Top-down map, multi-select, direct orders | 2D/3D mix, command abstraction, accessibility |
| Artillery | Costed support, radio request or map target | Observer requirements, range, cooldowns, gun assets |
| Conventional AI | Rules / utility-based strategic planning | What best fits existing engine AI? |
| LLM AI | Optional, strategic level only; robust fallback | Hybrid planning, safe tool interface, model budgets |
| War end | Defeat rival HQs / control required territory | Comeback rules, sudden-death prevention, endless variants |
| Persistence | Save strategic state first | Later persistence for destruction and terrain damage |
| Additional maps | Only after Everon works | Auto-generated strategic points and navigation validation |

---

## 16. Ideas the implementing AI is encouraged to add

Please feel free to suggest features or changes we have not considered. Some possibilities, **not commitments**, include:

- **Dynamic missions:** Patrol, escort, hold a crossing, ambush a convoy, rescue a squad, or defend an airfield, generated naturally from the changing war.
- **Morale and surrender:** Cut-off troops may withdraw or surrender instead of fighting to the last soldier.
- **Reconnaissance and intelligence:** Scouts, radio reports, stale enemy contacts, and uncertainty become strategically valuable.
- **Squad experience:** Veteran groups improve and become worth preserving.
- **Supply routes:** Dangerous roads and destroyed bridges affect resource flow or access to certain reinforcement types.
- **AI personality:** Aggressive, cautious, defensive, or opportunistic Commanders—whether rule-based or LLM-driven.
- **Civilian / Resistance dynamics:** Local control and civilian harm influence support or recruitment, if appropriate for the tone of the game.
- **Spectator / observer mode:** Watch a war between NPC factions and inspect Commander reasoning for experimentation and debugging.
- **Scenario seeds / presets:** Quick, replayable wars with different starting conditions.
- **Better UI feedback:** Player-readable radio messages, objective alerts, resource changes, and reasons why a command is rejected.

These ideas should compete for priority based on *playability, engineering cost, performance, coherence,* and the current state of the repository.

---

## 17. What success might look like

A compelling first version does not need perfect global simulation, procedural destruction persistence, or sophisticated LLM planning. It might already be successful if:

1. The player can launch Conquest on classic Everon and choose a faction.
2. Friendly and enemy NPCs move between several bases, fight, capture, and recapture them.
3. Owning more bases produces resources that visibly enable more troops.
4. Reinforcements obey logical, safe spawn constraints.
5. There is a coherent AI Commander even when the player never takes command.
6. The player can become Commander, issue group orders on a map, then return to combat.
7. Artillery works through a shared resource-limited mechanism.
8. A war can reach a legitimate outcome without manual scripting.
9. The game remains performant and doesn't break normal OFP gameplay.
10. The foundations could later accept an optional LLM Commander without redesigning the entire mode.

### Final invitation

**The proposal is deliberately open.** We would value a developer's or AI agent's independent assessment of what the engine can support today, what the smartest first vertical slice would be, which ideas should change, and what additional ideas could make the result more enjoyable. Please treat the checklist as a conversation starter. A better design is more valuable than literal compliance with this draft.

**The north star:** A believable, enjoyable, fully NPC-driven war on Everon that the player can experience either as a soldier or as the Commander—with optional language-model intelligence as an enhancement, never as a requirement.
