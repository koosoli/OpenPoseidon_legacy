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

The final engine source is `fd1e236c`; the installed acceptance stamp is
`f6e5c1d5` (documentation-only successor). Native geometry, admission,
selection and terrain checks passed: **143 cases, 24,368 assertions**.
Four new GPU ceiling/legacy/overlap/cap checks and seven existing hole renderer
tests passed earlier; selection and destruction fixes do not change shaders.

Original retail Everon, standard US soldier, no cave addon:

- Cave acceptance: `build/cave-editor/original-everon-cave-earth-clear-20261004-021607/result.json`.
  Normal W-key entry beneath the retained roof and W-key exit passed, together
  with 10 cm aperture side/height controls, stable-ID selection/rotation,
  occupied edit refusal, non-last deletion preserving the neighbouring cave,
  final removal and restoration of terrain queries. L activated the soldier
  flashlight during this final run; screenshots show a dim beam. An ordinary
  Light on action fallback is also supported by the runner.
- Trench acceptance: `build/trench-editor/original-everon-trench-earth-final-20261004-021336/result.json`.
  A read-only whole-island census selected a clear retail lane at
  x=5500, z=10000. No trees were removed. Infantry walked down/out; the actual
  M1Abrams drove down to the parking floor and reversed out with normal W/S.
  Its root dropped 1.94 m at 17.8 m along the trench. Both kinds passed
  selection/rotation, cut-edge refresh, occupied deletion/rotation refusal and
  removal/restored terrain checks. Rotation sweeping over an outside soldier
  was refused too.

Runtime testing found a production defect: generated earth models with low
mass were auto-classified as destructible trees. Abrams contact removed the
terrain cut. Generated cave/trench models now declare `dammage=No`, and the
native regression constructs a real ObjectPlain to verify DestructNo. The
final tank run demonstrates that the floor remains through actual contact.
Earlier tree-obstructed, steering and vanished-cut runs are failed diagnostics,
not acceptance evidence. The final cave runner walks the whole armed model
clear of the entrance before asking to delete it.

Both final runs preserved EXE/DLL hashes, exited normally, and contained no
GPU validation/device-loss/unhandled-exception errors. Visual inspection shows
functional but plain faceted cave/earth surfaces; material polish and a brighter
flashlight remain separate improvements. These are author-run receipts, not
owner visual approval. The curated JSON beside this guide preserves final
identity, movement and edit evidence without the large diagnostic censuses.

Installed runners (invoke through `scripts/with-game-lock.sh`):

```text
scripts/Test-CaveEditor.ps1 -ExpectedCommit <installed commit>
scripts/Test-TrenchEditor.ps1 -ExpectedCommit <installed commit>
```

Both use original retail Everon and a standard US soldier, isolate the profile,
preserve the installed pair, record real terrain queries and normal input,
capture screenshots, and clean up only the game process they start.
