# PERF-026 — A CPU draw call costs 0.2 µs, so the decal batcher is not worth building

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** PERF-024 (encode attributed: `enc: draw` is 6.4–6.8 ms), PERF-025 (2,572 of the
CPU-drawn objects are footprints)

## Why this was measured before it was built

PERF-025 named a lever — one instanced draw per decal model instead of one per decal — on the
strength of two facts: footprints are 60% of the CPU-drawn complement, and PERF-024 measured
recording draws at 6.6 ms. Putting those together *assumes the 6.6 ms is proportional to the
number of calls*. Nothing had tested that assumption, and the batcher is a real change to the
mark and road draw paths.

So the assumption was tested first, by deleting the draws instead of batching them.

## The instrument

`POSEIDON_MARK_TTL=<seconds>` sets the footprint lifetime in `SoldierOldMove`. The default is
**10**, the shipped value, so nothing changes for anyone who does not set it. `0` spawns no
footprints at all: the ablation arm. It is a measurement switch, not a gameplay setting.

## Measured (Everon `perf_combat`, 3441×1440, `POSEIDON_VSYNC=0`, three alternating runs per arm)

| | marks on (TTL 10) | marks off (TTL 0) |
|---|---:|---:|
| `direct_calls` / frame | 3,416 | **2,425** |
| `Worker enc: draw` (CPU) | 7.55 ms | **7.36 ms** |
| CPU frame | 21.20 ms | 21.24 ms |
| GPU frame | 19.05 ms | 20.37 ms |
| `swap` (ring wait) | 6.78 ms | 10.64 ms |

**991 fewer draw calls bought 0.19 ms.** That is **0.19 µs per CPU draw call**, so the whole
3,566-call complement is worth about **0.7 ms**, not 6.6. And the frame did not move at all
(21.20 vs 21.24): the scene is GPU-bound, and the producer simply spends the saving waiting
longer at the swapchain — `swap` grew by 3.9 ms, more than absorbing it.

## What PERF-024's 6.6 ms actually is

Not per-call overhead. `enc: draw` brackets the depth prepass **and** the whole 3D colour
sub-pass replay, water included: per-section state, `Load`/`EnsureUploaded` LRU marking (the
residual WLD-026 attributed to `drawSection`), and the retained path's own recording. Removing
28% of the calls left 97% of the time behind, which says the cost is per *section* and per
*state change*, not per `Draw3D` op.

## The decision

**The decal batcher is not built.** Its whole ceiling is ~0.5 ms of CPU on a frame with 6.8 ms
of GPU back-pressure already in it. The two cheaper alternatives PERF-025 offered the owner —
a mark cap and a shorter life — are withdrawn with it: they would change what the player sees
and buy nothing measurable.

This does not retire PERF-024's direction, it narrows it. "Fewer recorded draws" is still the
CPU lever, but the unit that costs is a **section**, not a call, and the CPU is not the bound
on this scene in the first place. The honest next target on `perf_combat` is the GPU.

## The general lesson

A per-item cost inferred by dividing a bracket by an item count is a hypothesis, not a
measurement. It took one env knob and six bench runs to find that this one was off by 9×,
against a change that would have taken a day and touched the mark and road paths.

Nothing here changes what the game draws: the default lifetime is unchanged at 10 s.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
