# Per-ammo drag coefficients for classic CWA content

BAL-010's first core item is a "verified drag / ballistic coefficient model". Until 2026-08-27
the engine hardcoded **one** coefficient, `-0.0005`, for every bullet and shell in the game
(`ShotShell::ShotShell`). A 5.56 and a 12.7 decelerated identically. No amount of solver accuracy
can make that realistic, because the thing being solved is the same for every round.

`Ballistics::ShellParamsFor` now reads an `airFriction` property from the ammo config and falls
back to the old constant when it is absent. This directory is the other half: the values, and how
they were arrived at.

## Why classic content needs an overlay at all

Measured, not assumed: `BIN\CONFIG.BIN` from a retail *ARMA: Cold War Assault* install
debinarises to 755 KB of text with an intact `CfgAmmo` of 82 classes, and contains **zero**
occurrences of `airFriction`. The property is an Arma-era addition; 2001 content predates it.

So shipped missions are byte-identical until someone supplies values. That is deliberate — it is
what makes the engine change safe to land on its own.

## The derivation

The engine's drag law is

```
accel = speed * |speed| * airFriction
```

which is `dv/dt = k·v²` with `k = airFriction`. Along the flight path that integrates to

```
v(x) = v0 · exp(k·x)        =>        k = ln(v / v0) / x
```

So a coefficient follows directly from **one published retained-velocity figure**: muzzle
velocity, velocity at a known range. No fitting, no free parameters. Every number below is that
formula applied to the real cartridge each CWA class stands in for, at 300 m.

| Class | Cartridge | v₀ (m/s) | v @ 300 m | `airFriction` |
|---|---|---|---|---|
| `BulletSingle` | 5.56×45 M855 | 940 | 700 | −0.000983 |
| `BulletSniper` | 7.62×51 M118 | 838 | 700 | −0.000600 |
| `Bullet7_6` | 7.62×54R (MG) | 830 | 690 | −0.000616 |
| `Bullet12_7` | 12.7×99 M33 | 890 | 810 | −0.000314 |
| `Bullet4x20` | 20×102 HEI | 1030 | 900 | −0.000450 |
| `Bullet4x23` | 23×152 HEI | 980 | 850 | −0.000474 |
| `Bullet30` | 30×173 HEI | 990 | 870 | −0.000431 |
| `Shell73` | 73 mm OG-15V | 400 | 370 | −0.000260 |
| `Shell105` | 105 mm APFSDS | 1450 | 1390 | −0.000141 |
| `Shell125` | 125 mm APFSDS | 1700 | 1630 | −0.000140 |
| `Heat` | HEAT, shaped charge | 900 | 850 | −0.000191 |

The ordering is the sanity check, and it is the right way round: a light, fast 5.56 sheds speed
fastest **per metre**, a heavy .50 far more slowly, and a long-rod sabot is nearly flat. The old
single constant of −0.0005 sits closest to a 7.62 — which is why it was never obviously wrong,
and why it was wrong for everything else.

Only base classes need values. CWA's `CfgAmmo` is a clean inheritance tree (`BulletSingleW`,
`BulletBurstE`, `Bullet7_6G` … all derive), so 11 entries cover all 82 classes.

## What these numbers are, and are not

They are **one-point fits to a v² law**, which is not what a real bullet obeys. Real drag
coefficients vary with Mach number, sharply around the transonic region, so a single exponential
cannot match a real trajectory at both 100 m and 800 m. Fitting at 300 m puts the error where
infantry engagements are rarest.

That is a limitation of the engine's drag law, not of these values, and it is the honest boundary
of this work: this makes the *relative* ballistics of different calibres correct, and the absolute
drop much closer than one shared constant. It does not make the engine a ballistics simulator.
Doing that means a Mach-dependent Cd, which is a model change, not a data change.

The shell and HEAT figures are the softest. Tank ammunition retained-velocity data is less
consistently published than small arms, and CWA's shells are generic stand-ins rather than
specific rounds. Treat those four as reasoned estimates; the small-arms rows are solid.

## Applying it

The overlay in `cwa-airfriction.cpp` is a config fragment, not a mod. It has to be merged into
the config the game actually loads:

```bash
PoseidonTools.exe config merge <game>/BIN/CONFIG.BIN docs/ballistics/cwa-airfriction.cpp -o <game>/BIN/CONFIG.BIN
```

**Back the file up first.** That rewrites data in the game install, and the install is not under
version control.

Nothing in the engine requires this. With the overlay absent every round keeps
`Ballistics::DefaultAirFriction` and behaves exactly as it did.
