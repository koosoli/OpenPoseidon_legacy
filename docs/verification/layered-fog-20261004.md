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

## Final closure repair and installed recheck

Engine source `5ee637d7` fixes the local-versus-final transport mismatch:
physically admitted foreground uses local transport, while synthetic legacy
far-cover uses the final-volume transport already used by the sky background.
Explicit open/closed endpoints avoid a one-ULP GPU mix cancellation. The complete
Game/Tests build passed; six actual GPU/shader tests passed, including exact
closure, gated foreground, roof/native/overflow/weather negatives and composed
production shaders. OFF still returns the original mixture before new reads.

The first installed attempt stopped at the unchanged cross-arm terrain-range
guard before capturing an ON view. Weather/range updates have distinct engine
boundaries; the runner now requires 30 actual rendered frames and three stable
consecutive range readings before freezing. It retains the later exact range
checks and timeout. No tolerance was weakened. That failed attempt remains in
the curated receipt, rather than being counted as acceptance.

The settled campaign `original-everon-fog-closure-settled-20261004-044419` passed
all four arms / twelve views. Equal-weather terrain/base/tactical ranges were
515.25 m in OFF and ON; zero-weather ranges were 1480 m. Native source, actual
weather/cameras, current encoding traces, normal owned shutdown and unchanged
matched-pair hashes passed. Manual Hable exposure stayed 0.15 in every arm.
Root inspected corrected ON/OFF ground, between and above views alongside the
previous failed contour screenshot. The previous hard distant terrain contour
is absent from the corrected above-layer view; the road remains readable and
additional depth haze is present. Canopy/card fog appearance, broad exposure,
interior and cost acceptance remain open. **Default remains OFF**; this is a
bounded correction, not general visual promotion.

Latest transport samples were 0.180-0.207 ms at 1280x720. Inclusive frame samples
were OFF 4.900-5.063 ms and ON 5.278-5.534 ms. The asynchronous single-sample
limitations above apply; these are not a statistical overall-cost result.

Installed stamp: `5ee637d7+dirty`, 2026-10-04 04:41 local. The dirty state at
deployment was only the rifle-acceptance Markdown correction; engine and
shader source matched the committed tip. Later runner/doc commits do not alter
that pair. EXE SHA256 `17B0A9F51C75CA6A5E18EE666F6E9567735ECABEC26378134A722C40F6D4712C`;
renderer SHA256 `40E217CEF85798E1B80E894CB9B733F4AE6826B51AE586C0BC3B14A8E21565FC`.
