# Boat-free title menu — 2026-10-07

Owner request: show aggressive seas without the boat, and remove the title scene
from the README feature lists. The source change is `a499f636` on
`codex/private-release-prep-20261006`.

The menu mission now contains only the retail-required player anchor, placed
on the island outside the view. The BoatW, crew and AI route have been removed.
The offshore camera stays at [2510,3518,9], aimed at [2250,3050,0]. Late afternoon,
the existing Tidewater Storm spectrum, amplitude 1.65 and foam 1.1 remain unchanged.
No renderer, wave budget or boat physics changes were made. Both README title-scene
feature bullets were removed; Tidewater remains described as an engine feature.

`Build.ps1 -Target PoseidonGame` passed. Locked `Deploy.ps1` installed both built
binaries and the authored content, stamping `a499f636`. The compiled version label
is `f4249ab`: configure ran before the content commit. Engine, app and build-source
files are identical between these commits; the menu files match `a499f636`.

Locked `Test-MenuSea.ps1 -ExpectedCommit a499f636` passed. Evidence:
[runtime receipt](tidewater-menu-20261007-runtime.json) and native captures in
`build/menu-sea/20261007-172052-072/`. First and restored menu captures were visually
inspected: animated storm water, no boat, low afternoon sunlight and the project
logo. Ordinary editor placement, mission Preview, abort and return to the title
menu preserved the process, window and windowed mode. The camera's horizontal
position stayed fixed; its `getPos` height is relative to the animated sea.
No persistent water profiles were written and no crash/GPU-validation errors
were detected. The menu override is still transient and clears during gameplay.

Two preceding test attempts exposed test assumptions, not scene failures: this
classic interpreter does not support `isNil`, and `getPos` reports a changing
sea-relative height for a camera whose absolute position is fixed. The runner
now checks the deployed mission has no boat definition and compares horizontal
camera coordinates. No engine workaround was added.

Both READMEs also explicitly describe Vulkan through WGPU, Direct3D 12 on Windows
and the separate legacy OpenGL renderer. The installed smoke log reports the
NVIDIA GeForce RTX 3070 Vulkan adapter. The eleven export guard tests passed.
Previous release exports remain historical snapshots; a future release candidate
must regenerate its inventory and export from the selected tested source pin.
