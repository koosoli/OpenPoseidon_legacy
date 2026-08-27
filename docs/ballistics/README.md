# Per-ammo drag data for classic CWA content

BAL-010's first core item is a "verified drag / ballistic coefficient model". Until 2026-08-27
the engine hardcoded **one** coefficient, `-0.0005`, for every bullet and shell in the game
(`ShotShell::ShotShell`). A 5.56 and a 12.7 decelerated identically. No amount of solver accuracy
can make that realistic, because the thing being solved is the same for every round.

`Ballistics::ShellParamsFor` now reads `airFriction`, and optionally `dragModel` +
`ballisticCoefficient`, from the ammo config, falling back to the old constant when they are
absent. This directory is the other half: the values, and how they were arrived at.

**Current state (2026-08-27, second pass).** The four small-arms classes are on a real drag
function with published ballistic coefficients. Everything else is still a constant fit to an
unsourced anchor and is marked as such. The first pass of this document fitted *every* class to
remembered retained-velocity figures; that turned out to be systematically too flat, and the
section "What the first pass got wrong" below keeps the measurement rather than quietly
overwriting it.

## Why classic content needs an overlay at all

Measured, not assumed: `BIN\CONFIG.BIN` from a retail *ARMA: Cold War Assault* install
debinarises to 755 KB of text with an intact `CfgAmmo` of 82 classes, and contains **zero**
occurrences of `airFriction`. The property is an Arma-era addition; 2001 content predates it.

So shipped missions are byte-identical until someone supplies values. That is deliberate — it is
what makes the engine change safe to land on its own.

## Two models, and which class gets which

### Small arms: a standard drag function

A real bullet's drag coefficient varies with Mach number, sharply through transonic. The BRL/McCoy
standard projectiles capture that as tabulated curves (`drag-functions/`), and a cartridge is tied
to one by a **ballistic coefficient**:

```
accel = -(rho * pi * Cd(Mach)) / (8 * BC) * v^2          BC in kg/m^2
```

The projectile's own diameter and mass cancel out of that, which is what makes a single published
BC enough. Coefficients below were cross-checked against the ammunition library shipped with
[gehtsoft-usa/BallisticCalculator1](https://github.com/gehtsoft-usa/BallisticCalculator1), a
JBM/McCoy-derived calculator; each of its entries carries its own source attribution.

| Class | Cartridge | v0 (m/s) | model | BC (lb/in^2) | source | v @ 300 m | v @ 600 m |
|---|---|---|---|---|---|---|---|
| `BulletSingle` | 5.56x45 M855 | 940 | G7 | 0.151 | standard published figure, cross-validated against G1 0.304 | 641 | 402 |
| `BulletSniper` | 7.62x51 M80 | 810 | G1 | 0.397 | BPN | 598 | 426 |
| `Bullet7_6` | 7.62x54R ball | 830 | G1 | 0.426 | Federal 146 gr FMJ (GRAU 7N1 measures 0.430) | 628 | 460 |
| `Bullet12_7` | 12.7x99 M2 | 890 | G1 | 0.670 | US Mil | 752 | 628 |

Each also carries an `airFriction` **fallback**, for a build that ignores the drag function. Those
are no longer guesses: they are least-squares fits of the constant law to the drag-function
trajectory over 0-600 m.

| Class | fallback `airFriction` | first-pass value | error in drag |
|---|---|---|---|
| `BulletSingle` | -0.001328 | -0.000983 | 35 % too little |
| `BulletSniper` | -0.001039 | -0.000600 | 73 % too little |
| `Bullet7_6` | -0.000954 | -0.000616 | 55 % too little |
| `Bullet12_7` | -0.000570 | -0.000314 | 82 % too little |

A constant law fitted this way still cannot track the transonic rise: at 600 m the fallback runs
about 5 % fast for the 5.56 and under 2 % for the .50. That is the price of the model, not of the
fit, and it is why the drag function is the primary entry.

### Everything else: constant drag, unsourced anchors

The engine's constant law is `dv/dt = k*v^2`, which integrates along the path to
`v(x) = v0 * exp(k*x)`, so `k = ln(v/v0)/x` follows from one retained-velocity figure.

| Class | Cartridge | v0 (m/s) | v @ 300 m | `airFriction` |
|---|---|---|---|---|
| `Bullet4x20` | 20x102 HEI | 1030 | 900 | -0.000450 |
| `Bullet4x23` | 23x152 HEI | 980 | 850 | -0.000474 |
| `Bullet30` | 30x173 HEI | 990 | 870 | -0.000431 |
| `Shell73` | 73 mm OG-15V | 400 | 370 | -0.000260 |
| `Shell105` | 105 mm APFSDS | 1450 | 1390 | -0.000141 |
| `Shell125` | 125 mm APFSDS | 1700 | 1630 | -0.000140 |
| `Heat` | HEAT, shaped charge | 900 | 850 | -0.000191 |

**Those seven anchors are not sourced.** They are reasoned estimates. They are correctly ordered
by calibre and they are a large improvement on one shared constant, but the small-arms block is
the warning: every one of *those* four remembered anchors turned out too optimistic, three of them
by more than half. Expect the same bias here, and do not cite these figures.

They deliberately do **not** get a drag function. G1 and G7 describe blunt-nosed and boat-tail
rifle bullets; an APFSDS long rod, a HEAT shell and a 40 mm grenade resemble neither at any scale,
so a BC for them would be invented data wearing a citation.

## What the first pass got wrong

The original table fitted all eleven classes to retained-velocity figures taken from recollection
-- for M855, "700 m/s at 300 m". The drag-function model says 641 m/s. Measured consequence, same
integrator, M855 from 940 m/s:

| Range | G7 0.151 | G1 0.304 | first-pass constant | legacy -0.0005 |
|---|---|---|---|---|
| 300 m | 642 m/s, 65 cm drop | 644, 65 | 700, 61 | 809, 55 |
| 600 m | 402 m/s, 360 cm | 416, 357 | 521, 308 | 696, 247 |
| 800 m | 307 m/s, 845 cm | 325, 818 | 428, 645 | 630, 472 |

At 600 m the first-pass 5.56 was 30 % too fast and 14 % too flat. The legacy shared constant was
73 % too fast and dropped barely two thirds as far. The lesson is in the method, not the numbers:
a remembered anchor cannot be cross-validated, and a fit to one is indistinguishable from a good
fit until something independent is put next to it.

## What these numbers are, and are not

The constant-law rows are **one-point fits to a v² law**, which is not what a real bullet obeys. Real drag
coefficients vary with Mach number, sharply around the transonic region, so a single exponential
cannot match a real trajectory at both 100 m and 800 m. Fitting at 300 m puts the error where
infantry engagements are rarest.

That was the honest boundary of the first pass. It no longer applies to the small-arms classes:
those now use a Mach-dependent Cd, so they track a real trajectory at 100 m and at 800 m rather
than at one fitted range. It still applies in full to every constant-law row above.

The shell and HEAT figures are the softest of all. Tank ammunition retained-velocity data is less
consistently published than small arms, and CWA's shells are generic stand-ins rather than
specific rounds.

## Applying it

The overlay in `cwa-airfriction.cpp` is a config fragment, not a mod. It has to be merged into
the config the game actually loads:

```bash
PoseidonTools.exe config merge <game>/BIN/CONFIG.BIN docs/ballistics/cwa-airfriction.cpp -o <game>/BIN/CONFIG.BIN --force-overwrite
```

**Back the file up first.** That rewrites data in the game install, and the install is not under
version control.

Two things about that command are not optional, both found the hard way on 2026-08-27:

- **`--force-overwrite` is required.** Retail config classes carry an access lock. Without the flag
  the merge is a silent no-op: exit code 0, a message saying it wrote the file, and a byte-identical
  output.
- **The overlay must not name parent classes, and the engine must be new enough to cope.** The
  parser drops a class whose base is absent from the same file, so an overlay can only write
  `class BulletSingle { ... }`. Until `ParamClass::Update` was fixed, that form *erased* the
  existing parent -- twelve CfgAmmo classes came out re-rooted, losing every inherited property and
  every child, with no diagnostic. If you are merging with an older build, verify afterwards:

```bash
PoseidonTools.exe config debin <game>/BIN/CONFIG.BIN -o merged.cpp
```

and check that `class BulletSniper: BulletSingle` still names its parent. A correct merge adds 21
lines and removes none.

## The inheritance trap

`ExplosiveBullet` descends from `BulletSingle`, so the 20 mm, 23 mm and 30 mm rounds are children of
the rifle bullet. A drag function is inherited like any other property and overrides `airFriction`
wherever it lands, so G7 + BC 0.151 on `BulletSingle` would make every autocannon round fly as a
62-grain bullet and silently ignore the values in the constant table. The overlay stops it with an
explicit `dragModel = "constant"` on `ExplosiveBullet`.

The shell and HEAT branch needs no guard: `Shell` and `Heat` descend from `Default`.

Nothing in the engine requires any of this. With the overlay absent every round keeps
`Ballistics::DefaultAirFriction` and behaves exactly as it did.
