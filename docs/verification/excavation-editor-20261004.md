# Cave and trench editor, 4 October 2026

Branch `codex/cave-editor-20261004` extends `codex/dec-integration-20261003`
at `ee869c1b`. It retains that integration and adds generated, original cave
and trench meshes; no external cave addon is required.

## Owner controls

Load a single-player mission, open Dev Tools with Ctrl+backtick (or Ctrl+semicolon),
then select **Map Editor**.

- **Horizontal cave**: start at the side of a hill, looking into rising ground.
  Defaults: 3 m wide, 2.5 m high, 16 m long. The closed back must have at least
  40 cm of terrain above the ceiling. An entrance floor offset is available.
- **Infantry trench**: choose gently sloping ground. Defaults: 1.5 m wide,
  12 m long, 1.6 m deep, with a 4.8 m access ramp and a flat bed.
- **Tank trench**: defaults: 5 m wide, 24 m long, 2 m deep, with a 12 m access
  ramp and 12 m parking bed. The entrance is open; the back is closed.
- Click **Place excavation (free-fly)**, point at land and click left mouse.
  The camera's compass heading determines the long axis.
- Wheel shrinks/enlarges the opening; Shift+wheel changes length. Cave width
  and height, and trench width, can reach **10 cm**. A soldier cannot fit in
  the smallest openings. The HUD shows dimensions and placement refusals.
- **Undo last excavation** removes the last cave/trench. Leave the excavation
  first; closing a hole on an underground player is refused.
- Use **Placed excavation** to select an existing generated cave or trench.
  Change **Selected heading**, then click **Apply selected rotation**.
  Its entrance stays fixed. **Delete selected excavation** removes that item,
  including older placements. Rotation preserves its stable ID and validates
  land, ceiling/rim and other holes before changing the scene. Both the old and
  proposed footprint must be clear of the player/controlled vehicle.
- **Soldier flashlight**: press L (the Headlights binding) while on foot.
  The ordinary **Light on** action is another way to activate it. The beam
  follows your view, reaches about 20 m and remains active in daytime caves.
  In free-fly, use Dev Tools → Zeus → Head torch →
  **Head torch (follows the view)** instead.

The ordinary circular height brush remains available for foxholes/craters.
Its default requested radius is now 8 m instead of 64 m, its wheel/slider minimum
is 10 cm, and **holding Shift while painting multiplies the rate by four**
(bounded to the existing safe per-transaction displacement). The actual radius
of height painting remains at least 0.75 terrain grid cells. Shaped excavations
provide sub-grid openings without distorting the surrounding heightfield.

## Current boundaries

This is single-player, mission-local authoring. Reloading removes the generated
excavations; save/export, connected segments and AI Paths are not implemented.
There is a shared eight-excavation cap. Trenches currently require rim heights
within 25 cm of their level entrance; shorten the trench or choose gentler
ground if placement is refused. Underwater floors and overlapping excavations
are refused.

Cave geometry has five independent convex collision slabs, a roadway floor,
an open entrance and a bounded terrain cut. It preserves mountain surfaces and
grass above the ceiling. Trench geometry has an open top, convex earth banks,
a solid ramp/floor and matching roadway. Legacy soldier/vehicle collision is
the supported traversal path. Box3D's cached terrain heightfield still has no
cavity representation; loose bodies and articulated corpses are not certified
for these excavations. Direct lateral cave sunlight remains conservative.

## Verification

Native geometry, admission and terrain checks: 139 cases, 24,339 assertions
passed with the final tank preset. Four new renderer tests passed, including actual GPU
ceiling/legacy/overlap/cap checks; seven existing hole renderer tests passed.

The installed original-Everon cave runner passed against engine source
`3d196ce3`, using runner revision `739cc40b`. The installed stamp carried
`+dirty` because the ledger/docs and Cargo's incidental lockfile adjustment
were uncommitted; no engine/shader changes differed from `3d196ce3`.
The first runtime test found and corrected a compass-heading sign error.
The second verified the corrected footprint/floor but exposed a diagnostic
error: `IntersectWithGround` returns a point beyond the requested segment on
miss, not a negative sentinel. The witness now checks the actual returned point
against the requested segment. A third attempt exposed retained camera aim
in the test setup; using the ordinary LookCenter key before movement corrected
the steering. Failed attempts are not acceptance evidence.

Passing receipt: `build/cave-editor/original-everon-cave-20261004-010300/result.json`.
The stock SoldierWB walked with normal W input from outside to 6.82 m inside,
at ASL 43.9186 m beneath terrain at 47.1964 m, then returned outside with W.
The centre floor/camera support was 43.92 m; the retained surface above was
50.371 m. A genuine 0.1 m by 0.1 m aperture passed side/ceiling controls on a
6.25 m terrain grid. Undo restored terrain ray hits/support without changing
raw terrain heights/revision. The EXE/DLL hashes stayed unchanged and the
game exited normally without GPU validation errors. Screenshots show a plain,
box-shaped portal and a dark interior; natural cave surface/lighting polish
is beyond this prototype. A second passing run at
`build/cave-editor/original-everon-cave-flashlight-20261004-011308/result.json`
also pressed the ordinary L key indoors during daytime: the player flashlight
activation was logged and its screenshot showed illuminated walls/floor,
although the beam remains dim. All entry/exit, tiny aperture and undo checks
passed again.

The first centred trench run verified normal infantry entry/exit and undo,
but its Abrams approach was blocked by retail trees 9 m before the entrance
(`build/trench-editor/original-everon-trenches-20261004-010908/result.json`).
It never entered the trench and is not tank acceptance evidence. The revised
runner rejects conservative bounds of retail obstacles throughout the approach
and trench; no trees are deleted for testing. The clear-ground tank rerun and
the owner-requested selection/rotation/deletion checks are pending.

Installed runners (invoke through `scripts/with-game-lock.sh`):

```text
scripts/Test-CaveEditor.ps1 -ExpectedCommit <installed commit>
scripts/Test-TrenchEditor.ps1 -ExpectedCommit <installed commit>
```

Both use original retail Everon and a standard US soldier, isolate the profile,
preserve the installed pair, record real terrain queries and normal input,
capture screenshots, and clean up only the game process they start.
