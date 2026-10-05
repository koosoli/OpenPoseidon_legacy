# Local terrain mission editor - 2026-10-05

Implemented on `codex/local-map-mission-editor-20261005`. Installed compiled source
`ed1c10a6` at 23:11 Luxembourg time. Later commits contain tests/documentation only.

## Backup before implementation

- Pushed `codex/backup-before-local-map-editor-20261005` at `a48ce457` to the safety mirror.
- Created and verified complete-history Git bundle:
  `C:/Users/mail/.codex/backups/OpenPoseidon-before-local-map-editor-20261005.bundle`
  (`git bundle create --all`, successful `git bundle verify`, 885 refs).
- Bundle covers committed Git history/refs, not ignored installed game assets.

## Behavior

Local Content > Maps > select supported terrain > Open selected enters the OFP 2D
mission editor in the existing process/window. A loading screen remounts assets.
Place OFP units, groups and waypoints; normal Preview uses the selected terrain.
The existing vehicle-browser restart is preserved.

Owned JSON descriptors persist under `GamePaths::UserDir()/LocalMapWorlds`.
Stable `OP_Local_<hash>` aliases retain terrain identity in mission folder extensions;
sentinel world names route loading to the selected source. Generated config classes
inherit OFP weather/UI defaults without modifying Eden. False inherited Eden town
labels are suppressed and the editor centers on actual terrain bounds.

To reopen a saved imported-map mission after restarting, select the same terrain in
Local Content and use the editor's Load button. Sources must remain installed.
Only the currently mounted imported terrain is offered in editor mission selection.
Original foreign missions/gameplay configs are not imported; modern material/LOD
compatibility remains partial, especially Reforger vegetation.

Deferred asset remounts run between frames. Old world workers are destroyed before
file-bank/native-mount replacement. Failed content reload restores previous
roots/alias/mods. Closing the imported-map editor restores original OFP content and
island availability. Reforger world selection refreshes per load rather than keeping
a static first-world snapshot; native model loader/mount are cleared on teardown.

## Verification

- `scripts/Build.ps1 -Target PoseidonGame` passed.
- Registry actual-source syntax and focused descriptor checks passed: stable IDs,
  case-insensitive sentinel lookup, JSON round trip/atomic replacement, missing
  sources, corrupt metadata and path validation.
- Installed `Test-LocalMapEditor.ps1` full round trips passed for Stratis windowed
  and native Reforger Everon borderless fullscreen.
- Both placed playable `SoldierWB` through the unit dialog, saved `.OP_Local_*`,
  reloaded, entered Preview, returned to editor, exited and recovered exactly the
  original island list. PID, native window handle and display mode stayed unchanged;
  normal exit and runtime/GPU checks passed.
- Receipts: `local-map-editor-stratis-20261005-runtime.json` and
  `local-map-editor-reforger-20261005-runtime.json`.
- Local captures/logs: `build/local-map-editor/20261005-231959-583` (Stratis) and
  `build/local-map-editor/20261005-232136-609` (Reforger).

Initial test iterations corrected harness expectations (island control 101,
visibility string 1 and default native unit combos); deployed implementation did
not require further changes. Screenshots establish terrain editor and playable
Preview, while existing Reforger rendering limitations remain.
