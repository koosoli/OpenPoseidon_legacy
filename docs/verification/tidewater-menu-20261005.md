# Tidewater title-menu scene

Implemented on `codex/tidewater-menu-scene-20261005`; installed compiled source
`59690124`. Runtime [receipt](tidewater-menu-20261005-runtime.json).

The standard main menu now follows an AI-crewed `BoatW` on a repeating offshore
route in the existing Tidewater storm spectrum. The authored scene uses September
15 at 16:40, moderate clouds, little fog and no rain. A steady camera follows the
hull from 9 metres above sea level. The scene references the locally installed
retail boat and Desert Island; no original archive or third-party asset was added.

The ocean look is a transient renderer override: Tidewater native, Storm sea,
amplitude1.65 and foam coverage1.1, without frozen water/cloud animation or glow.
It keeps the existing quality/FFT/geometry budgets. World cleanup clears it;
per-map profile serialization uses the underlying user settings. A mod's own
resolved menu intro stays in use, and data without Desert Island/the authored
scene keeps the original fallback.

Installed build and native harness checks passed: boat moved242.96m in the
observed interval, remained undamaged, and sampled offshore ground was-100.03m.
Actual scene captures show the hull riding breaking swells with low afternoon
light on the water. Ordinary editor unit placement and Preview, pause/abort,
editor exit, and restored menu all passed in the same process/window. No
water-look profile files were created. The sample rendered58.43FPS under a60FPS
cap at1280x720, high preset/MSAA4. This is a bounded observation, not an FPS
promise for other resolutions/hardware/profiles.

Commands: `scripts/Build.ps1 -Target PoseidonGame`, locked `scripts/Deploy.ps1`,
and locked `scripts/Test-MenuSea.ps1 -ExpectedCommit 59690124`. Native captures
are in `build/menu-sea/20261005-235528-240/{00_menu,01_menu_swell,02_mission,03_restored_menu}.png`.
Both first and restored menu images were inspected. There were no GPU validation,
device-loss, script parse or crash errors. The retail `pbr.p3d` reports its existing
unnamed hitpoint selection diagnostic; the boat continued driving without damage.

To see it, start the installed `OpenPoseidon.exe` normally and stay on the main
menu. The patrol boat and waves animate continuously behind the menu/logo.
