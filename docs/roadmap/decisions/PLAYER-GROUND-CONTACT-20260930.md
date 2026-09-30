# Player boot contacts and third-person visibility

Source baseline: root 821784e5, matched installed source identified by root as
654. No game, GPU, deployment or full build was performed by this agent. User
reports ordinary player walking makes no visible sand marks and the player can
vanish in third person, particularly on sand. **Installed reproduction and
acceptance remain pending**; previous AI-walking results cannot establish this.

## Concrete source defects and changes

`DrawSortObject` inferred cockpit/body routing from selected LOD == InsideLOD.
`ManType::InitShape` defaults `_insideView=0` when a model has no VIEW_PILOT
level, so that equality can hold for an ordinary third-person visual level0.
It is not proof of an inside view. World::Draw already explicitly forces the
inside LOD for the actual inside carrier; its external branch uses automatic
LOD selection instead. The new `ExplicitInsideViewDraw` requires the **forced**
and selected LOD to match the valid inside LOD. This also preserves inside
camera effects, which use that same forced queue. It changes no body alpha,
mesh, stance, physics height or weather shader. This is a verified routing
defect; it has not yet been shown to be the sole cause of the owner's symptom.

Physical footprints formerly lived entirely inside primary animation
`GetSoundEnabled()` and an `else if` selecting a single sound edge. The outgoing
secondary animation contributed real movement during transitions but emitted
no physical contacts. The new actual `GroundBootEvents` helper separates
physical sole edges from sound enable for genuine forward/strafe animations,
handles loop seams and both edges in a tick, and chooses the dominant animation
through a blend. Audio behavior remains in its existing sound block. Physical
calls use the actual animated memory sole (`stopaL`/`stopaP`), exact terrain/road
support and existing MudContact/SandContact admission; there are no synthetic
path stamps or new timer-driven stationary contacts. Silent static/reload poses
without stride produce no contacts. Dead actors and nonfinite soles fail closed.

Sand already admits the exact authored tuples Sand/ps, SandAbel/pi and
SandDark/pt with sand sound and empty character. These are stock terrain source
identities, not a Nogova world-name policy. Its outer-12%-of-every-tile ban
nevertheless created multi-metre no-mark bands even between two pure sand tiles.
That arbitrary band is removed. Every support tile still needs **all four exact
quadrants** to be admitted sand, with the whole .6m footprint/rim support's
roadway, snow, land/sea, slope and real sole-height checks. Mixed grass/road/rock
tiles still fail; SandBuilding and native/A3 mixed masks remain outside this
bounded physical producer. This is not all visually sandy ground acceptance.

Mud admits exact stock Field/pol/dirt/empty-character on any map. The previously
audited O.pbo pole1/pole2 fallback was additionally tied to `noe/noe.wrp` and an
arbitrary tile band. The actual source asset and unchanged Default metadata now
provide authority regardless of which OFP map references those same assets.
The compatibility `StockNogovaMudSoil` name remains for existing renderer setup
and harness callers, delegating to `StockCultivatedMudSoil`; the contact producer
calls the new helper directly. No generic Default, "dirt" sound-only, brown
ground, Village/bah, MudBuilding/rock, cutout character/clutter, guessed native
layer or texture basename is admitted. All actual footprint support, sheltered
soil, road, slope, sea and snow gates remain. Broader unknown soil families need
their own authoritative source proof; this patch does not assert every surface
on every map is deformable.

## Actual-player diagnostics and required installed check

`POSEIDON_PLAYER_GROUND_TRACE=1` opts into independent facts for the **actual
PlayerOn**, rather than substituting an AI walker:

- `PLAYER_GROUND_QUEUE`: camera enum, forced/inside LOD, residency/invisibility,
  retained coverage and actor world pose, every .5 simulation seconds.
- `PLAYER_GROUND_DRAW`: actual Man::Draw level/model/pose, camera, terrain height,
  physical mud/sand offsets, dead/hidden state, every .5 simulation seconds.
- `PLAYER_GROUND_SECTION`: deduplicated actual WGPU body/proxy source, camera,
  pass hint, primary mesh identity, real indices/flags/blend/depth/alpha,
  skin/CPU-pose/palette/conform state and finite matrix (max64 keys). It records
  a queued draw, not GPU pixel acceptance. Existing clothing admission/wetting
  functions remain owned by the separate uniform agent.
- `PLAYER_GROUND_BOOT`: actual memory sole, primary/secondary movement/factor,
  manual state, class/files/sound, terrain/road delta, active fields and actual
  changed-field result (max256 player events). Missing markers/nonfinite soles
  have explicit refusal lines. Existing MUD_STEP/SAND_STEP formats are unchanged.

Root must run a private ordinary-player mission using real keyboard/manual
movement, not `doMove` on a separate AI. At the previously verified Noe sand
source (around X2675 Z5125), require actual player displacement, boot events,
`sandChanged=true`, actual signed physical samples and visible player pixels in
both first and third person. Repeat on wet cultivated soil (around X4975 Z4675)
with real liquid rain/wetness, requiring `mudChanged=true` and real support
depth. Include standing still, turning/strafe, pause/resume and camera toggles;
standing/pause cannot manufacture contacts or physical field changes. A dry
soil/road negative and a real raised roof wet-soil negative remain necessary.
These coordinates are existing supported fixtures, not a promise about a new
manual mission's spawn, model or final ground height.

Use queue -> draw -> WGPU section -> actual image evidence to isolate remaining
invisibility. If no boot events occur, inspect the real player's movement and
memory-marker trace; if events occur with unchanged fields, inspect the exact
support tile/road/shore/sole rejection. Do not dismiss either report merely
because an AI fixture or a helper test passes.

## Completed checks

`scripts/Test-PlayerGroundBoot.ps1 -Compiler 'D:/Program Files/LLVM/bin/clang++.exe'`
passes under `-Wall -Wextra -Werror`:

- Actual inside-queue helper covers fallback0 automatic versus forced,
  distinct view LOD, mismatched/sentinel levels and camera-effect queue form.
- Actual boot helper covers sound-disabled forward/strafe, dominant outgoing
  blend, loop seams, both sole edges, idle/reload/pause and invalid inputs.
- Actual event helper drives actual SandField and MudField signed kernels;
  compaction/deep pressure and finite gradients occur without uniform lowering.
- Complete existing sand field/source and mud field/source regression suites
  pass, including persistence, caps, signed rim/gradient/snapshot parity and
  conflicting hard/grass/clutter metadata.
- Source call-site checks require both real animation producers, actual memory
  soles, explicit queue routing and preserved full sand support proof.

Full game compilation and installed ordinary-player/appearance verification are
owned by root and pending. No accepted rendering or manual gameplay claim is
made from these CPU/source checks.
