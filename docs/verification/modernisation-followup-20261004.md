# Modernisation follow-up: finite experimental checkpoint

Working branch: `codex/modernisation-followup-20261004`, safety-pushed.
Backup branch: `codex/backup-excavation-20261004`, immutable source `8b93dec7`.
The owner checkout and its unrelated changes were preserved.
Installed matching EXE/renderer: source `f29455a8`, 10:29 local on 4 October.
Owner-authorised default ragdolls and current toggle acceptance are recorded in
`automatic-ragdoll-default-20261004.md`. The current fog/weather/corpse feedback receipts are in
`weather-fog-corpse-feedback-20261004.md`; earlier checkpoints below remain
historical evidence.
Final twelve-view installed fog comparison passed and the inspected distant
contour defect is removed. The owner subsequently approved volumetric fog as the
default; automatic ragdolls are also default ON by owner request. See `fog-ragdoll-owner-revision-20261004.md`.

## Completed and installed

- Horizontal caves and straight infantry/tank trenches, small shaped openings,
  Shift rate, stable-ID selection, rotation and selective deletion with occupied
  bounds protection. Actual stock soldier cave entry/exit and M1 Abrams trench
  entry/exit passed on original Everon. See `excavation-editor-20261004.md`.
- Automatic stock Western infantry deaths connected to the existing Box3D
  physics world: articulated bodies/joints, physical falling, retained frozen
  skin/equipment/bounds, save/load, deletion cleanup and a shared runtime toggle.
  Four simultaneous standing deaths, a genuine AK74 projectile kill, save/load
  and active/frozen toggle retention passed. Broad physics regressions passed
  74 cases / 13,806 assertions. See `automatic-ragdoll-20261004.md`.
- Physically based experimental fog: two feathered world-altitude density
  layers, Beer-Lambert attenuation, directional single scattering and matching
  sky/cloud/surface transport. Current shader/GPU tests and installed controls
  are recorded in `layered-fog-20261004.md`; appearance remains experimental.
- Relevant bounded upstream ports reviewed against public main `b36cddc7`, the
  fetched PR heads and the supplied audit. Native tests and all four installed
  imported fixtures passed, including drop height, fired nearest object, Turbo
  and ordered notebook audio. See `upstream-followup-20261004.md` and
  `../roadmap/modernisation/upstream/UPSTREAM-RUNTIME-INTEGRATION-20261004.md`.
  Existing weather, grass and combat improvements were preserved.

## Test controls

Launch installed `OpenPoseidon.exe` with `--dev`.

- **Ctrl+backtick** or **Ctrl+semicolon -> Map Editor**: choose cave, infantry
  trench or tank trench. Place in freefly with LMB, wheel adjusts the opening,
  Shift+wheel adjusts length. Select an existing excavation and use its heading,
  rotation or deletion controls. Check the occupied guard by standing inside it.
- **L** toggles the soldier flashlight; the action menu also offers Light on.
- **Ctrl+F8 -> Physics -> Automatic infantry ragdolls (experimental)** enables
  automatic ragdolls (now enabled by default). Kill stock US infantry on supported dry ground. Turning
  it off preserves existing bodies and stops new admissions.
- **Ctrl+F8 -> Weather -> Fog** selects Legacy or Volumetric layered fog and
  edits fog amount immediately. Presets offer low ground fog, two layers,
  an elevated band, valley mist and mountain mist. Expand Fog layers and lighting
  for terrain following, valley density, patch scales, edge softness, layer heights,
  extinction, albedo and forward scattering. Overcast edits also apply immediately
  and share world weather with Zeus. Weather order is Wind, Fog, Rain, Snow,
  overcast/forecast, diagnostics. See `weather-fog-corpse-feedback-20261004.md`.

## Explicit remaining limits

Volumetric fog and automatic ragdolls are ON by default by owner request. Automatic
ragdoll floor registration costs about 231-247 ms / 16 MiB on Everon; the
current path prepares it at enable/first live simulation before combat. Unsupported
models, multiplayer, water, ladders, attached actors, excavation contact, unsafe
initial contact/speed and exhausted budgets retain authored death behavior.
Actual crouch/prone pose acceptance is unproved. There was no Box3D dependency
update. Existing manual prototype support is retained independently.

Excavations remain mission-local single-player prototypes; reload discards them.
Save/export, connected segments, AI routing and Box3D cavity contact are not
certified. Trees are not arbitrarily deleted by the editor.

The reported Everon floating houses at `[5805.63,3597.97,46.35]`, az147.8/el6.3,
remain unidentified and unreproduced. The exact view and correctly aimed close
views were inspected; twelve sampled candidate foundations were supported.
Distant coarse terrain can theoretically expose foundations, but the investigated
far candidates are hidden behind native hills from that camera. No unjustified
placement, global snapping or elevation change was made. The house report remains
open; see `../roadmap/modernisation/EVERON-PLACEMENT-DIAGNOSTIC-20261004.md`.

Upstream proposals requiring platform execution, renderer adaptation, RNG
isolation or ordering/performance proof remain conditional in the integration
guide. Retail game assets were not added to Git. Full ledger validation still
stops on older stale RND-005A and AST-011C evidence outside this reviewed slice;
those hashes were not blindly renewed. Author-run checks are not owner review.

This batch stops at the requested bounded checkpoint. It does not declare the
whole modernisation roadmap complete or silently extend it into another phase.
