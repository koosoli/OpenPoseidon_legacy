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

## Verification completed

Installed source234795e7, matched EXE/DLL deployed15:32. Full Build.ps1 succeeded.
17 actual layered-fog tests and2 boot integration tests passed. The actual
material flags and player-ground checks also passed in the child worktree.
The headless actual sky projection checks lower-hemisphere exclusion, unchanged
full-sphere energy and angular continuity across the old nadir basis threshold.
The actual encoded fog volume checks the bound SH ambient contribution, extinction
invariance and native/roof/source refusal contracts.

Owner launcher supports `-BootRelief` with `-VehicleClass Truck5t`. Drive forward
and reverse over wet tracks, leave the truck and walk fresh mud, then inspect new
boot marks at1–3m. The marks retain their existing short lifetime. Prototype
normal/parallax does not alter collision; tyres, lateral slip and other vehicle
classes are not added by this follow-up.

## Installed results and limits

- Original Everon26 actual moving poses passed at the reported road location:
  top-down250m/fog0.35 and oblique77.15m/fog0.78. Known black road rectangle absent.
  The supplied ugly owner pattern was not reproduced exactly; GPU tests prove
  corrected ground-crossing and sky-probe discontinuities, not universal absence.
- Same top-down fixture12 published timing samples: fog0.424 to0.454ms, sky
  projection0.051 to0.058ms, total GPU6.593 to7.050ms. No FPS improvement claim.
  Removing eight sky fetches does not guarantee a faster whole pass/frame.
- Default stock Truck5t with no prototype flags: real-rain8.61m drive,14.1cm rut,
  actual6/6 admitted wheels at roughly0.93m/s2 rolling resistance, normal exit.
- Actual stock SoldierWB walking on Nogova cultivated soil: dry/roof negatives,
  changed exposed contacts,19.9cm physical depression, unchanged paused support
  grid and actual BOOT_RELIEF_PROTOTYPE material admission. Close and fog captures
  inspected. Fine sole detail is present; owner aesthetic judgement is pending.
  Existing large surrounding puddle/wet-surface transitions remain visible and
  are not claimed polished by this prototype. No tyre profile parallax was added.
- Full raw receipts are retained locally under the paths in the committed compact
  runtime receipt. The boot fixture emits existing no-shape aiming diagnostics
  around its temporary roof control; no new renderer validation/panic/device loss.

This is the finite stopping point requested by the owner: installed code,
repeatable prototype launcher and bounded verification, with explicit remaining
visual limitations rather than extending into generic tyre/soft-body work.

Run `pwsh -NoProfile -File scripts/Show-BootReliefPrototype.ps1` from this checkout
to open the default Truck5t with optional boot relief enabled. This uses an
isolated graphics profile and holds the deployment lock until the game exits.
