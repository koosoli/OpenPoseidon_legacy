# Fog, boot relief and truck default — 2026-10-05

Bounded follow-up to the reported camera-following fog pattern and the approved
vehicle rut experiment. The exact owner pattern has not yet been reproduced with
the isolated moving top-down Everon fixture; no universal flicker-free claim.

## Changes

- `921ef114`: clip downward fog intervals to native air before selecting their
  density participant. GPU altitude sweep retains the same ground fog column.
  Stock SP Truck5t wheel rutting and admitted-contact longitudinal resistance are
  now enabled without environment flags; explicit zero disables either.
- `d53f8cbd`: optional authored boot-mask relief. See the boot prototype note.
  `POSEIDON_BOOT_RELIEF_PROTOTYPE=1`; changed mud/sand physical contacts only.
  Existing decal darkness/alpha infer a 3 mm decorative height, normal gradient
  and bounded UV parallax. This is not a measured/scanned sole depth texture.
- Fog ambient no longer takes eight sky probes in a camera-relative basis with
  a hard near-nadir basis switch and discrete hemisphere rejection. The actual
  sky projection computes additional upper-hemisphere SH9 coefficients with the
  same environment reads and reused 9 KiB workgroup scratch. HG convolution uses
  degree weights 1/g/g², no Lambert factors or upper-sky renormalization. This is
  a smooth band-limited ambient approximation; sharp sky detail is filtered.
  Existing full-sphere surface coefficients, direct sun, lamps, shadows, optical
  depth, fog layers and weather controls retain their roles.
- Vehicle runtime fixture uses800m view distance to keep the natural-rain wait
  independent of unnecessary distant rendering. No soil wetness setter/stamp.

## Verification in progress

Full matched EXE/DLL build and installed original-map smoke tests are required.
The headless actual sky projection checks lower-hemisphere exclusion, unchanged
full-sphere energy and angular continuity across the old nadir basis threshold.
The actual encoded fog volume checks the bound SH ambient contribution, extinction
invariance and native/roof/source refusal contracts.

Owner launcher supports `-BootRelief` with `-VehicleClass Truck5t`. Drive forward
and reverse over wet tracks, leave the truck and walk fresh mud, then inspect new
boot marks at1–3m. The marks retain their existing short lifetime. Prototype
normal/parallax does not alter collision; tyres, lateral slip and other vehicle
classes are not added by this follow-up.
