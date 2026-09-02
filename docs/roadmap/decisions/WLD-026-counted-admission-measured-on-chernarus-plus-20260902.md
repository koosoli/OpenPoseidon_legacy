# WLD-026 — Counted admission measured on Chernarus+: same fill, same frame, and a resident set that repeats

**Date:** 2026-09-02
**Branch:** `asset-obs-001`, binary `eeb532fd`
**Follows:** WLD-025 (the pose and the `land:obj` question), AUD-001 A1 (the two wall clocks),
commit "Object-stream admission is counted, not timed"

## What was asked

The roadmap's Chernarus+ target gated any further `land:obj` work on replacing the admission
wall clocks with counted budgets "so two machines report one number". The clocks are gone
(`WGR_OBJECT_STREAM_ADMIT`, default counted; `WGR_OBJECT_STREAM_COLD_PER_UPDATE`, default 2;
prefetch velocity on `Glob.time`). This is the measurement the commit said it still owed.

## Setup

WLD-025 pose (`5704.58 2523.37`, alt 120, az 260.9, hour 12), DayZ addons, `perf_combat`
running on top, 90 s warmup, two runs per arm, one machine (RTX 3070), bench window size.

## Results

| arm | updates | stopped by | resident | cold models | CPU frame avg | p95 | GPU | `land:obj` |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| counted (default) | 240 | cold budget 239 | 19,923 | 478 | 35.50 | 37.00 | 34.9 | 4.7–4.8 |
| `=timed` | 271 / 284 | ceiling 233/250 + budget 37/33 | 19,923 | 478 | 35.84 | 37.53 | 34.8 / 30.6 | 4.6–4.7 |
| counted + lockstep | 240 | cold budget 239 | 19,923 | 478 | 36.12 | 38.34 | 35.5 | 4.5–4.6 |

**Same fill, same frame.** Both modes reach the same resident set and pay for the same 478
cold models; the frame is GPU-bound at this window size (GPU ≈ CPU frame), so the admission
mode cannot move it and does not. `land:obj` reads 4.5–4.8 ms in every arm — larger than
WLD-025's 2.65 ms because this is a wider window and the fill is still running at 90 s; it is
the same number in every arm, which is the point.

**The clocks were deciding almost every update.** In timed mode 233–250 of 271–284 updates
ended on the 16 ms ceiling and 33–37 on the 4 ms budget: the rate, and therefore the resident
set on a given tick, was a function of the machine on ~95% of updates. In counted mode 239 of
240 end on the cold budget, and the fill takes fewer updates to converge (240 vs 271–284).

**The resident set now repeats.** The per-update residency rows (`resident`/`created`/
`released`) compared run against run:

| pair | result |
|---|---|
| counted + lockstep, run 1 vs 2 | **EQUIVALENT, 240 rows, identical row for row** |
| counted, no lockstep env, run 1 vs 2 | **EQUIVALENT, 240 rows** |
| timed, run 1 vs 2 | diverged at row 5: created 23 vs 21, released 11 vs 9 |

Two runs of the timed mode disagree about what the world contains by the fifth update; two
runs of the counted mode never disagree, and did not need the lockstep environment to agree
— the fill is a function of the tick sequence alone once no clock is consulted.

## What this closes and what it does not

Closes the gate on the Chernarus+ target: the number is the same on two runs, and by
construction on two machines. Does not answer whether 20,000 resident objects is the right
budget for a 3-million-placement world (WLD-025's open question), and does not measure a slow
machine, where the counted mode admits the same objects and the frame takes what they cost;
that trade is stated in the code and is the owner's to feel.
