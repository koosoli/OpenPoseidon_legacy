# Selective upstream review (2026-09-08)

Reviewed against upstream/main `3541affa` after fetching the owner's remote.
No merge of the upstream branch and no GL33-only bulk port.

## Ported and checked

`a61130ca` (scene-preloader reset): adapted to our UnloadGameData teardown.
The shared singleton previously kept its initialized flag after the Scene died.
Added the upstream preload-count harness query and remount regression flow.
Installed wgpu EXE/DLL (`2abd7b1b+dirty`, 19:48 deployment) exercised an actual
in-process remount through the TCP harness: 20 populated slots before, remount
returned OK, 20 after, orderly exit 0. Game and unit-test targets build.
Private reproduction: `.tmp-rotor-smoke/remount.ps1` and `remount.log`.
The Trident flow is added but the installed run used the equivalent direct harness
queries because the local orchestrator is not built and searches old binary names.

## Already represented / not applicable as-is

- `08e67e93`: VertexTable constructors already initialize _minMaxDirty and copy it.
- `2054dbc8`: MotionEdge::cost is already short in this fork, not char.
- `fa3a65dd`: Viewer already owns the RString returned by GetName across comparisons.
- `9a298d09`: its faulty nested AnimatedMinMax branch inside AnimatedBSphere does
  not exist here. Our method already respects isAnimated; do not transplant the
  upstream land-clip implementation over this fork's conform-plane path.
- `ed6119f9`: actual detail-texture release/load change is GL33-specific. Wgpu's
  texture bank has no matching cached _detail/_specular/_grass/_waterBump quartet.
- `6f0d7fec`, `bc3014a9`, `a918a023`, `315733ab` and GL33 state-cache changes:
  GL resource ownership/state fixes, not wgpu ports.

## Candidates requiring a separate benchmark / regression pass

- `cc4bd699` / `3541affa`: opaque shape buckets and blend-list filtering could
  reduce CPU sort cost, but this fork's scene walk has retained-GPU coverage,
  conform planes and extensive pass instrumentation. Not a conflict-free patch;
  price the remaining CPU fallback before replacing its ordering.
- `c551e783`: local-light instancing is implemented through GL UBO light indices.
  Wgpu has its own GPU light path; copying those APIs/indices is not an improvement
  by itself and risks changing the shared draw contract.
- `7ca93a57` and the land-clip instancing series: this fork already has
  DeformsSharedShape and a different GPU conform path. Compare predicates and
  destruction captures before any additional adaptation.
- `4c6aff84` / `078bb2f7`: cursor-layout fixes are potentially useful at ultrawide,
  but touch this fork's separately modernized UI scaling. Not ported blindly.

This is a targeted renderer/shared-lifecycle review, NOT a claim that every older
upstream commit or every non-renderer change has been exhaustively audited.
No change can be promised regression-free; measured selective ports are the gate.
