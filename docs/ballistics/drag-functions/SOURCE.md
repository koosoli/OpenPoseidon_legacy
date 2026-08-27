# Standard drag functions

`G1.txt` and `G7.txt` are tabulated drag coefficients Cd against Mach number for the G1 and G7
*standard projectiles*.

**Source.** Originally produced by the US Army Ballistics Research Laboratory (BRL); the
tabulations here were retrieved 2026-08-27 from JBM Ballistics' download page
(<https://jbmballistics.com/downloads.html>, files `mcg1.txt` and `mcg7.txt`), which attributes
them to BRL and to the late Robert L. McCoy, author of *Modern Exterior Ballistics*. BRL work is
US government output.

Stored verbatim, two columns, tab-separated: Mach, Cd. Do not "tidy" the values — they are
measurements, and the transonic rows in particular are closely spaced on purpose.

## What each one describes

- **G1**: flat-based projectile, blunt ~1-calibre nose. Peak Cd 0.6625 at Mach 1.40.
- **G7**: boat-tail spitzer. Peak Cd 0.4043 at Mach 1.05, and much lower everywhere subsonic
  (0.12 against G1's 0.20-0.26).

G7's standard shape matches 1985 military rifle ball (M855, M80, 7N1, .50 M33). G1's does not,
which is why a single G1 ballistic coefficient for those bullets drifts with velocity and G7 does
not. G1 data is nevertheless far more widely published — that tension is real, and the resolution
is to carry BOTH functions and let each cartridge use whichever it has a trustworthy BC for.

## Cross-validation performed 2026-08-27

The retardation formula intended for the engine is

```
a = -(rho * pi * Cd(M)) / (8 * BC) * v^2          BC in kg/m^2
```

which follows from `a = rho*v^2*Cd_bullet*A/(2m)` with `Cd_bullet = i*Cd_std` and
`BC = m/(i*d^2)`: diameter and mass cancel, leaving only the standard function and the BC.
(`1 lb/in^2 = 703.07 kg/m^2`.)

Integrating M855 from 940 m/s at sea level with **both** functions and their respective published
BCs:

| model | BC | v @ 100 m | 200 m | 300 m | 400 m | 500 m | 600 m |
|---|---|---|---|---|---|---|---|
| G1 | 0.304 | 833 | 735 | 644 | 559 | 482 | 416 |
| G7 | 0.151 | 834 | 734 | 642 | 556 | 476 | 402 |

The two agree to **0.3 % at 300 m** despite using different tables and different coefficients.
That is the check worth having: a systematic error in the formula would move both, but only a
correct BC↔Cd normalisation makes two independent (function, BC) pairs land on the same
trajectory. They diverge at 600 m, which is exactly where they should — G1 mis-describes a
boat-tail once it is well transonic.

## Consequence for `../cwa-airfriction.cpp` — those values are PROVISIONAL

The constant-k values in that file were fitted to a retained-velocity figure of **700 m/s at
300 m** for M855, taken from recollection rather than a source. The cross-validated drag-function
model above says **~643 m/s**. If the drag-function model is right — and two independent
confirmations say it is — then the fitted coefficients are systematically **too low in drag** and
every round in that table is flatter than it should be.

They are still a large improvement on one shared constant for all ammunition, and they are still
correctly *ordered* by calibre. But they should be re-derived by running this model per cartridge
with its published BC and reading the anchor off the result, instead of off a remembered range
card. That was not done here because it needs trustworthy per-cartridge BCs, and inventing those
would repeat the mistake at one remove.

**Nothing in the engine consumes these tables yet.** The flight law is still constant-Cd.
