# Finite ASL layered fog checkpoint

The opt-in WGPU candidate implements two fixed world-altitude layers with
feathered density, Beer-Lambert transmittance and directional single scattering.
It uses current weather, sky lighting and conservative physical coverage, and
applies matching transport to sky/clouds and surface consumers. It does not
change tactical visibility. It is not a complete coupled atmosphere or interior
multiple-scattering model.

Launch with `--dev`, open **Ctrl+F8 -> Sky -> Layered low fog (experimental)**,
then enable **Physical ASL fog layers**. The startup option is
`WGR_LAYERED_FOG=1`; optional layer profiles are documented in
`docs/layered-fog-finite-asl-20261004.md`. **Default remains OFF.**

## Installed author-run controls

Two campaigns passed on original Everon: default grading against installed
`b1d87cb1`, and an explicit identical manual Hable exposure of 0.15 against
`3de356cb`. Each used four isolated arms: OFF/ON at weather fog 0.45, then
ON/OFF at zero weather fog, with ground/between/above cameras. The ASL profiles
were chosen once from the first native terrain observation and held fixed.

All twelve views per campaign retained the native source, unedited terrain and
matching requested camera/weather. Existing tactical/terrain/object ranges
agreed at equal weather. Current encoding traces admitted transport for ON/0.45
and rejected it for OFF or zero weather. All arms exited normally without GPU or
unhandled exceptions; the installed EXE/DLL hashes remained unchanged.

Each view waited at least 256 rendered frames and captured the actual published
performance response before/after its screenshot request. In the first campaign
the ON transport samples were **0.188-0.204 ms** at 1280x720. Inclusive whole-frame
samples were mixed: OFF 4.967-6.096 ms, ON 4.878-7.022 ms. These asynchronous
single samples are descriptive, not a statistical overall-cost certification.
They exclude extra coverage/background/consumer work from the transport timer;
no nested timer sums or exact screenshot/GPU-frame identity are asserted.

Curated campaign receipts: `layered-fog-20261004-runtime.json`. Raw sidecars,
actual encoding traces, logs and PNGs remain under ignored `build/layered-fog/`.
Source checks also passed seven production shader compositions, Sky pipeline
creation, existing water raster controls, three actual fog GPU regressions
(including finite/reversed-depth handling), and 13 current ABI21 refusal/matching
tests. The complete paired game/renderer build passed.

## Appearance and promotion decision

Root inspected the actual ground and above-layer OFF/ON images. Default midday
grading clipped much of the road/horizon in both arms. The fixed-exposure control
made those surfaces readable and showed additional depth haze, but also a hard
distant contour in the above-layer ON view. Appearance is **not accepted for
default promotion**. Coverage boundaries, legacy distance-fog composition,
interiors/canopies and a quiet representative cost campaign need further work.
Those limits are preserved rather than treating a passing encoding trace as
visual acceptance. This is a finite experimental implementation for subsequent
owner testing, with ordinary launches keeping the existing fog path.
