# Coherent grass gust fronts — 2026-10-05

Owner-requested source implementation by grass_wind_waves subagent, integrated
with the fog budget follow-up. Default: travelling fronts enabled, variation1.0,
length45m along wind and coherence135m across it. Dev Tools Grass tab beneath
gust speed: Travelling gust waves toggle, Local gust variation0..1, Gust front
size10..200m. Disabling restores the old isotropic coordinates/amplitude; zero
variation evens gust amplitude while direction/flutter retain natural motion.
Grass reset and general settings reset construct fresh default GrassSettings.

Three existing noise evaluations remain three; no added draw/compute pass or
texture fetch. Near blades, mid blades, photo cards and shadows consume the same
field. Renderer-private input tint alpha lanes were unused for shading; they
carry sanitized controls into existing uniform spare lanes without ABI growth.
The front heading is frozen at the first valid wind; continuous velocity
integration advects world positions without rotating kilometre-scale absolute
noise coordinates when weather changes. Pause/rewind/gap behaviour stays bounded.

Subagent actual wind.rs rustc tests3/3 passed; shader colour/shadow expression
parity and unchanged3 noise calls verified. Full renderer GPU/layout checks,
matched build and installed capture are pending. No universal FPS promise.


Actual renderer grass GPU/layout/pipeline tests15 passed, including both shaders,
4x/1x cards and colour/shadow uniform offsets. Full matched Build.ps1 succeeded.
Installed a9683298 stock Everon capture at owner field5018.51/4087.66,
altitude100m/elevation-18, measured wind override8m/s bearing220/gustiness0.7:
6 spaced captures300/360/420/480/540/600, normal shutdown, populated170835 mid
instances, no shader validation/device/panic failure. Evidence directory:
build/grass-stability/gust-fronts-default-OwnerEveron-1-20261005-172817.
This is bounded runtime/visual capture acceptance; broad artistic tuning across
all weather speeds/maps remains available through the live controls.
