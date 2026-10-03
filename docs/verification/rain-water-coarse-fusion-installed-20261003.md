# Installed coarse runoff fusion evidence, 2026-10-03

The installed Noe occupied UH60 Dry/Rain campaign passed with normal zero
exits and unchanged executable/DLL hashes. Actual rainy solver receipts use
the fused path; dry receipts use the existing empty shortcut. This supports
the production promotion's functional and dispatch checks. It does not
establish a causal game frame-rate gain.

Installed source: `fcb9ad7b1fe3b5f000009c8e6ae2dcb407fb522d`, stamped
`codex/streaming-residency fcb9ad7b 2026-10-03_21:07 deploy-ps1`.
Executable SHA256: `E94F92CDBB09D8E92C53502634B1E28C520C1E9983D09F3FB4365F8E88A33540`.
DLL SHA256: `9110F082F4CFEECB1E7E2A1667A206C792675DCCEE1EEFD17BE209C19F6A5331`.

## Actual receipt and raw-state checks

Authoritative campaign:
`C:/Users/mail/OneDrive/Documents/GitHub/CWR-CE-paavohuhtalafork-/.claude/worktrees/combat-audit/build/rain-helicopter/fused-runoff-installed-quiet-20261003-210821-579dfc/`.
The campaign `result.json` SHA256 is
`35DE0D8DEAE675E799995E940A80AA14F71D88FB1B5CB8A081A806AE2E557016`.

Independent read-only inspection used the existing exact capture helper:
raw hash, designated path, paused clock, current native source, grid,
generation/revision, and canonical binary64 pending/budget bits all match.
Both captures are `RWCAP001`, 513 by 513 at 25 m spacing, with an 88-byte
header and 2,105,440 total bytes. Native terrain is 2048 at 6.25 m spacing,
sampled at stride 4, with 263,169 exact native bed witnesses. Both captures
have generation 3/revision 275, source revision 2, and fine mode off.
Each capture preserves its arm's actual state before/after capture; the
different process world tokens are not interpreted as identical worlds.

| Readback | Dry | Rain |
|---|---:|---:|
| Finite, nonnegative depth cells | 263,169 | 263,169 |
| Wet cells | 0 | 148,653 |
| Maximum depth, m | 0 | 0.11121830344200134 |
| Derived retained volume, m³ | 0 | 94,507.06982082466 |
| Captured cells outside fused guard bounds | 0 | 0 |
| All reported steps | 274 | 274 |
| All reported empty steps | 274 | 0 |
| All reported fused steps | 0 | 274 |
| All reported cached raw/limited steps | 0 / 0 | 0 / 0 |
| Complete six-phase reports | 69 | 69 |

All six phases preserve their names/order and common counts; raw equals
limited, and raw plus fused equals steps minus empty in every report. These
counts include setup/warmup and are separate from the contained measurement
windows below. The source bed payload is identical between arms, SHA256
`96F1BFC48FE2085D02CA6793E8A77B2FBD1413F6E9DE58D51864C0AB4BD061D2`,
with bed range −166.04998779296875 to 744.4963989257812 m.

Raw Dry capture SHA256:
`29F6A3EED643338E3F51EF56478CD3197187889967484D57C5DC1F0F7D9DF803`.
Raw Rain capture SHA256:
`A4F624DB0B1F18C22A4BDD718B9A560112FD4FD9040F024631EC9015FC354F5C`.

Summing the native binary32 depths in order reproduces the captured Rain
volume. Its volume minus cumulative rain less infiltration, evaporation and
outlet is −0.03845889589865692 m³, approximately 2.57e−7 of cumulative rain
input; this is an observed floating-point budget residual, not exact mass
equality. Dry has zero volume/residual; its tiny positive rain input is fully
absorbed. Dry is a live initialized field, not a disabled-physics control.

There are 25 positive-depth cells at or below the captured current sea level:
8.51374547892192 m³ total, maximum depth 0.0014744163490831852 m. Their beds
are 2.924999713897705..2.924999952316284 m versus current sea 2.926290512084961 m.
`SetSeaLevel` updates the level without clearing cells; outlet processing
occurs in the next genuine Step. Captured pending time is 0.01667025312781334 s,
below the 0.25 s step. This endpoint observation is therefore not treated as
evidence that fusion introduced a sea-outlet failure.

The read-only audit output is saved locally at
`C:/Users/mail/.codex/worktrees/rain-water-coarse-replay-20261003/build/installed-fusion-audit/result.json`.
No clock, field, game, device, compiler or timing benchmark was run by this audit.

## Contained installed measurements

The campaign's `contained-cost-comparison.json` selects reports whose whole
previous-to-current reporting interval lies inside the first/last actual
measurement samples. It excludes partial boundary intervals. These are wall
elapsed timings of the coarse Advance scope, not exclusive thread CPU time.

| Measurement | Prior afffcc53 Dry | New fcb9ad7b Dry | Prior afffcc53 Rain | New fcb9ad7b Rain |
|---|---:|---:|---:|---:|
| Fully contained reporting windows | 23 | 23 | 23 | 22 |
| Advances | 1,387 | 1,387 | 1,389 | 1,328 |
| Steps | 93 | 92 | 93 | 88 |
| Empty steps | 93 | 92 | 0 | 0 |
| Fused steps | 0 | 0 | 0 | 88 |
| Coarse wall ms per Step | 1.336983 | 1.532122 | 9.959474 | 5.215910 |
| Maximum coarse Advance ms | 1.9584 | 2.2233 | 11.8415 | 8.5520 |
| Advances over 50 ms | 0 | 0 | 0 | 0 |

New Rain raw-transfer and limited-flux calls are zero; all 88 measured
nonempty steps are fused. Its fused phase totals 320.3737 ms (3.64061 ms per
Step). Total coarse scope falls 47.63% in this separate-flight comparison.
Dry is 14.60% slower here, so no dry-speedup claim is made. The earlier quiet
counter-free offline ABBA comparison of identical restored actual states
remains the controlled optimization evidence: 16.47% lower rainy-state
solver time, bitwise equal observable output, and no accepted dry gain.

| Completed frame measurement | Prior Dry | New Dry | Prior Rain | New Rain |
|---|---:|---:|---:|---:|
| Frames | 2,697 | 2,645 | 2,390 | 2,393 |
| Average ms | 9.124198 | 9.306060 | 10.328125 | 10.257763 |
| p95 ms | 10.539 | 11.019 | 13.238 | 11.960 |
| Maximum ms | 29.658 | 30.244 | 32.813 | 33.324 |

Both new completed frame captures are ordered and drop-free. Rain average
frame time is only 0.68% lower and its maximum is higher. A lower solver
scope or p95 in one before/after route does not prove a causal/global FPS
improvement. Published GPU/encode snapshots may lag or repeat and are not
summed into a same-frame timing claim.

The new actual Dry/Rain route spans 25.653/25.619 simulation seconds with
102.234/101.569 m XZ displacement. Across 22 paired samples, maximum XZ
difference is 1.05778 m, height difference 0.62569 m and velocity difference
0.05635 m/s. Requested route, ranges and overcast match, but physical scene
parity is not exact. The old/new Rain endpoint differs in liquid rain
(0.7994223 versus 0.8723939), tide (3.0483644 versus 2.9262905 m) and retained
volume (89,255.37 versus 94,507.07 m³). These are separate live flights, not
an identical captured-state A/B; no fallback or all-map guarantee is inferred
from the captured native 25 m field. Fallback/guard transitions remain covered
by the separate O0/O2 observable-equivalence tests.
