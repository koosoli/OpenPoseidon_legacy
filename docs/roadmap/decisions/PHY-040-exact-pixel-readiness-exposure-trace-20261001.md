# Exact corpse capture readiness and opt-in exposure tracing

The installed `e5445111` repeat
`build/corpse-pose/final-pose-stable-baseline-20261001-055625-45b72c`
failed **before any held-pose or final-pose query**. All eight retained PNGs
had simulation time 17.612; no three consecutive hashes matched. The owned
process exited normally with the installed pair unchanged. This establishes
query-independent image drift and does not accept the corpse diagnostic's
complete runtime gate.

Readback timestamps span 05:57:11.677 through 05:57:14.296 (2.619 seconds).
Adjacent image pairs changed 361, 152, 149, 126, 72, 90 and 38 pixels,
respectively. Every changed RGB channel differed by at most one unit; each
adjacent pair's total absolute channel difference equaled its changed-pixel
count. First versus last changed 671 pixels, total absolute difference 672,
with signed RGB sums [-49,-111,-290]. Changes are scattered across the wall,
ground and actors, not a geometric movement restricted to the corpse. These
measurements use Pillow/NumPy on the actual retained PNGs, not renderer output
fabricated for a test.

The render-time glare recovery has a nominal 1.8-second constant, but its frame
delta is capped at 50ms. Thus 2.619 seconds of wall time need not equal 2.619
seconds of its adaptation on a slow renderer. This is a source-supported
candidate, not an actual cause attribution. The downward camera's current
sun focus should be zero; whether an earlier camera left a recovering glare
multiplier must be measured. Ordinary tonemap and GTAO dither have no changing
frame term. The strict eight-capture bound and exact pixel gate stay unchanged.

## Diagnostic source

The only renderer addition is an opt-in `EngineWgpu::NextFrame` log block,
after this frame's cameras and light inputs have been prepared but before
its renderer queues are submitted. `WGR_EXPOSURE_TRACE` accepts a decimal
integer 1..6000 as a frame interval; unset, zero, negative, trailing junk,
overflow and values beyond 6000 disable it. `1` logs each frame; `30` samples
every thirtieth prepared frame.

Each `Exposure trace:` row records the current snapshot generation,
simulation/time-of-day, actual HDR/automatic-tonemap flags, actual exposure
enabled/key/min/max/rate/derived-rate/tau, manual exposure, glare and their
pushed product. It also records camera position/direction, byte-exact hashes
of all submitted projection/view matrices and camera xyz positions, actual
legacy and pushed sun/ambient, sky-light policy, night/cloud state and pending
screenshot state. Numeric lighting/exposure/camera values have nine decimal
places. Read actual `enabled`; the stale initialization comment in
`EngineWgpu.cpp` is not authoritative. Current `ExposureSettings` defaults in
`Engine.hpp` enable adaptation, with minScale 0.10 and maxScale 1.10.

`previousCompletedScale` uses the existing blocking `GetAutoExposureScale`
readback. It is the actual stored renderer scale from the completed frame
before the current prepared queues are submitted; it is **not** attributed
to the current snapshot. The first sample may be the renderer's neutral
constructor seed of 1. `blockingReadbackMs` records the measurement's wait.
The trace deliberately drains overlap and perturbs pacing. It is diagnostic
evidence only, never performance evidence. No exposure, glare, camera, shader,
metering, ordinary snapshot or rendering arithmetic is changed. With the
environment unset there is no readback, hash loop or logging.

## Parent-owned installed checks

After the matching build/deployment, run the same corpse wrapper with
`WGR_EXPOSURE_TRACE=1`, preserving its existing auto-exposure OFF and manual
exposure 1 controls. Retain raw trace and screenshot-completion timestamps.
Compare the successive paused rows: manual/glare/product/scale, matrix and
camera-position hashes, time-of-day, sky policy and pushed light values. If
glare changes while all other inputs remain fixed, it supports that candidate;
if glare is exactly 1 and stored scale exactly 1 throughout, it rejects it.
Do not expand readiness duration or relax equality before that measurement.

The same trace can accompany the sunny Church snow campaign with its actual
default exposure controls. Record whether adaptation is enabled, whether the
actual scale reaches its minimum, whether legacy or sky radiance feeds the
frame and whether the pushed manual/glare product is expected. A min-clamped
scale plus very large radiance differs from a disabled adaptation path and
requires a different correction. A dim rotor snow frame alone does not
establish sunny Church exposure acceptance. No darker material grade or
ordinary exposure-policy change is part of this diagnostic candidate.

Source review confirms one diagnostic hunk, its bounded parse, readback before
`MoveQueuesInto` and existing exposure API usage. `git diff --check` passes.
Full C++ build and installed trace results are pending root ownership. Moving
corpse deltas, contacts and calibrated articulated solver admission remain
blocked by the exact read-only readiness gate.
