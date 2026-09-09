# Arma 1/2 missions from an external installation

Assessment against asset-obs-001, 2026-09-10. This is a migration plan, not a
claim that an original mission is already playable. No source assets were copied
into the engine or game directory during this inspection.

## Evidence and available content

Inspected the owner's root `inspiration/officialarma` read-only with the existing
`PoseidonTools pbo list/show` commands, rather than extracting whole archives.

- A1 `ALDP_A1_PBOs_APL-SA_part1/missions.pbo` contains the Armory mission
  `armory1.intro/mission.sqm`, `init.sqf` and its script tree. Its init immediately
  invokes `execVM`. This archive is not evidence of the A1 campaign being present.
- A2 `ALDP_A2_PBOs_APL-SA_part2/missions.pbo` contains Bootcamp, scenarios and
  campaign mission directories, including `c3_manhattan.chernarus` and
  `bootcamp/t01_basic_training.utes`. These are actual mission resources, not
  merely models or terrain samples.
- The inspected A2 basic-training init uses `sleep`, `createDiaryRecord`,
  `execVM` for the MP framework, post-process commands and numerous `kbAddTopic`
  calls referring to `.bikb`, `.fsm` and `.sqf` files. Even this training mission
  is not a minimal expression-interpreter smoke test.
- OA/DLC mission archives also exist, but their content has not been audited yet.

There is enough local content to start a dependency/command census now. A retail
installation is useful later for exact patch parity, missing A1 campaign content
and reference gameplay. It is not a blocker for this planning work.

## Existing integration points

| Layer | Actual code | Consequence |
|---|---|---|
| External archives | `Foundation/Platform/AppConfig.cpp` registers `--addon-root`; `Core/ModCollection.cpp::ResolveAddonArchiveRootList` resolves archive folders. | Read-only external asset mounting already exists. Do not introduce an asset-copy/conversion prerequisite. |
| VFS/config mount | `Core/GameState.cpp::LoadAddonArchiveRoots`, `Asset/Addon/AddonSystem.cpp::ParseAllAddonConfigs`. | Reuse PBO prefixes and required-addon ordering; verify precedence and generation-specific base configs. Mount success is not config compatibility. |
| Mission selection | `apps/cwr/Game/GameApplication.cpp::StageTestMissionForGame`. | Current test path extracts a PBO or copies a loose mission to a temporary directory. It is not a selector for a mission nested inside a multi-mission archive. |
| Mission entities | `AI/ArcadeTemplate.cpp`, including required-addon checks. | Audit original mission fields/classes before spawning. Do not silently substitute OFP soldiers for unsupported Arma classes. |
| Startup script | `UI/DisplayUI.cpp::RunInitScript`. | `init.sqs` uses the scheduler; `init.sqf` currently executes directly and unscheduled. A mission that sleeps during init needs real suspend/resume semantics. |
| Scheduler/save state | `Game/Scripting/Scripts.cpp::ProcessLine`, `SimulateBody`, `Serialize`. | Current persisted program position is SQS line/label based. Scheduled SQF needs its own resumable execution state, not line splitting or rewriting scripts as SQS. |
| Command surface | C++ registrations in `Game/Commands` and the expression evaluator. | Repository C++ search found no `execVM`, `execFSM`, `kbAddTopic` or `kbTell` implementation/registration. Must establish semantics and coverage, not add success-returning stubs. |

Paths in the table are relative to `engine/Poseidon` unless explicitly under apps.
World/model/material readers are useful foundations; their presence does not prove
animations, turrets, injuries, tasks, conversations or mission endings work.

## Recommended stages

1. **Preflight without running a mission.** Catalog mission subdirectories directly
   inside mounted PBOs; record world, required addons, entity classes, init files,
   script/FSM references and unresolved commands. Emit a machine-readable manifest
   and a readable unsupported-feature report. Hash sources, write only to the user
   cache. Dynamic script generation means static coverage is a lower bound.
2. **An isolated content profile.** Select OFP, A1, A2 or OA as the compatibility
   environment, using an external game root. Preserve existing OFP configuration
   precedence. Do not mount every generation into one shared namespace: world
   names such as `Intro` and common config classes can collide. Validate missing
   installation/version dependencies before changing the active world.
3. **Archive-backed mission context.** Carry archive identity, mission-relative root
   and content profile through mission selection, `description.ext`, localization,
   script loading and save/load. Resolve relative paths against the mission root,
   absolute virtual paths against the mounted installation. Keep saves/caches out
   of that installation. Do not extract a whole multi-mission archive merely to
   choose one mission.
4. **Scheduled SQF vertical slice.** Preserve the OFP SQS baseline. Implement parsed
   scheduled execution with call frames, locals, handles, suspension, cancellation
   and bounded work per simulation tick. Then add the exact unsupported commands
   needed by the smallest chosen original mission. Test scheduled versus
   unscheduled behavior, errors and save/resume explicitly.
5. **One whole unmodified A1 mission, then one A2 mission.** Choose from the actual
   census, not from title familiarity. Verify briefing, player/AI creation,
   movement, weapons, objectives, death, restart and successful ending. A2 Bootcamp
   needs conversation/FSM/task integration; defer Manhattan until a smaller
   mission passes because its campaign/module dependencies are much broader.
6. **Broaden compatibility deliberately.** Data-driven mission FSM execution and
   the `.bikb` conversation system are separate from redesigning soldier AI with
   HTN/BT. Add modules, multi-turret/animation cases, campaign state and multiplayer
   only with separate gates. Keep the original OFP mission suite passing.

## Acceptance and risks

- A loader screenshot is not a playable-mission acceptance. Require the original
  mission's own ending and a mid-mission save/load followed by the same objective
  progression. Record unsupported calls with source file and line.
- No original asset redistribution in fixtures or commits. Commit synthetic test
  inputs and manifests/tools, not the inspected archive contents.
- Main risks: config/name collisions, unsupported SQF scheduling semantics,
  script-driven addon initialization, conversation FSMs, animation/skeleton
  differences, vehicle behavior, task/UI state and persistent campaign state.
- Keep script side effects on the authoritative simulation thread. Multiplayer
  requires command locality, event order and mission state replication; a working
  SP mission does not imply MP compatibility.
- No timeframe or percentage compatibility claim until the preflight census and
  first mission run expose the actual denominator. Do not replace missing game
  behavior with no-op commands to make a mission appear to start.

Recommendation: incremental compatibility adapters and a scheduled-script slice,
not a new engine or wholesale AI replacement. The external-mount requirement is
largely infrastructure already present; mission behavior is the substantial work.
