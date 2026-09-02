# REN-VEG-004 — The retained-tree sawtooth: what the governor trace shows, and what it will take

**Date:** 2026-09-02 (late)
**Branch:** `asset-obs-001`
**Follows:** REN-VEG-003 (the governor works again for CPU-drawn worlds)

## The trace

The capture JSON now carries `objects.lod_governor`: the frame duration the loop measured, the
duration it estimated, the complexity sum and its retained term, the four-frame history, the
target band, the LOD bounds and the iterations spent. Chernarus+, WLD-025 pose, trees on the
retained path (`WGR_TREEADV_RETAINED=1`):

| | value |
|---|---:|
| profiler frame avg / p95 | 15.4 / 46.7 ms |
| governor `frame_ms` (last 4 frames) | 30 |
| governor `estimated_ms` | 31 |
| complexity / retained / history mean | 665 k / 466 k / 651 k |
| iterations spent | 5 (never converges) |
| `_lodInvWidth` | 0.093 (the clamp) |

The loop is not pinned. It lives on a **five-second sawtooth**: the estimate carries *last
frame's* retained triangle count flat across its five iterations, so a refine step cannot see
that DayZ's tree ladder — 4k / 2k / 1k / **6** triangles at resolutions 1..4 — multiplies the GPU
cost 700× on the way down. The frame goes 13 → 55 ms in one step, the coarsen step that follows
arms the oscillation guard (`_lastScaleWorseTime`, a 5 s lockout on refining), and the loop sits
at the cards for five seconds before trying again. The 4-frame average the governor reads (30 ms)
is the spike plus three card frames; the profiler's 256-frame mean (15 ms) is mostly cards.

## What was tried

**Refine slowly, coarsen fast** — refinement capped at one +5% step per frame, coarsening
untouched. Result: p95 50 ms, five iterations a frame, same histogram. The step size does not
matter when the estimate is blind to the step's cost; and the damping would have made every
world slower to recover detail after a loading spike. Reverted in the same commit.

The retained term was also made sticky (the readback lands every few frames) — correct, kept,
not the cause.

## What it will take

The estimate needs a **per-level cost prediction for the retained set**: per-LOD triangle
totals from the cull's own counters (it already keeps a per-level instance histogram), shifted
by the candidate detail multiplier — the ladder is linear in resolution (1, 2, 3, 4), so a
candidate `l' = l·f` moves an instance at level *k* to roughly level *k·f*. With that in the
loop the refine step that costs 55 ms is visible *before* it is taken, and the loop can stop at
the level whose cost fits the band (level 2 at ~4.6 M triangles is the candidate on this
scene). Alternatively, register vegetation with a LOD bias so its ladder is spread over
distances the loop can walk one level at a time.

Until then: trees stay on the CPU (Chernarus+ default 31 → 20.5 ms with the governor of
REN-VEG-003), and `WGR_TREEADV_RETAINED=1` remains the lever for the one number the CPU
cannot give — `land:obj` 7.65 → 1.05 ms.
