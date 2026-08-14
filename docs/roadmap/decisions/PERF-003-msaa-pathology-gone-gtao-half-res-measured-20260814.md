# PERF-003 — Half-res GTAO is worth 1.66 ms, the MSAA pathology is gone, and the grass arm measured nothing

- **Ticket:** PERF-003 (roadmap §2.1, the owner's top complaint)
- **Date:** 2026-08-14
- **Branch:** `dayz-compat-layer` (the tree had moved; see below)
- **Follows:** PERF-002, which measured the pre-change frame
- **Status:** measured. No code changed.

## Conditions

Stratis, 1080p, free-fly camera `2446.86 4413.66 67.97 115.5 -8.2`, hour 12, six arms
interleaved within each round, three rounds. Binaries a matched pair built two
seconds apart (exe 00:46:55, dll 00:46:53) and launched through the harness, not out
of the retail install — the failure mode CORE-NEG-003 documents.

Spreads 0.3–7.2%, the tightest this project has recorded. The machine was quiet: the
owner was asleep and the other four agents had finished.

| arm | r1 | r2 | r3 | median | spread | vs base |
|---|---:|---:|---:|---:|---:|---:|
| base | 11.72 | 11.87 | 11.76 | **11.76** | 1.3% | — |
| `WGR_GTAO_SCALE=1` (full res) | 13.40 | 13.42 | 13.44 | **13.42** | 0.3% | **+1.66** |
| `WGR_ABLATE=planar` | 10.65 | 10.20 | 10.38 | **10.38** | 4.3% | −1.38 |
| `WGR_MSAA=1 WGR_ABLATE=planar` | 10.57 | 10.14 | 10.43 | **10.43** | 4.1% | −1.33 |
| `WGR_MSAA=1` | 11.34 | 10.54 | 11.09 | **11.09** | 7.2% | **−0.67** |
| `WGR_ABLATE=grass` | 11.49 | 11.54 | 11.57 | **11.54** | 0.7% | −0.22 |

## Half-res GTAO: confirmed, 1.66 ms

Forcing full-resolution GTAO costs **+1.66 ms** against the shipped half-res default,
and the cost lands exactly where it should: the `AO horizon march` region goes
**0.63 → 2.34 ms**, a 1.71 ms move that accounts for the whole frame delta. A region
that moves by the same amount as the frame is what separates a real saving from a
coincidence, and this is the arm with the tightest spread in the batch (0.3%).

That answers half of what PERF-002 left open. The half-res default is worth keeping.

## The MSAA pathology does not reproduce

PERF-002 measured `WGR_MSAA=1` at **+33.05 ms** against a 34.36 ms base — fewer
samples costing 33 ms more, which was never plausible as a real cost. The standing
hypothesis was that at 1 sample the planar reflection renders directly into mip 0 of
a mipped `TEXTURE_BINDING` texture (`ensure_planar_target` picking `mip_views[0]`),
defeating framebuffer compression.

**It is gone.** `WGR_MSAA=1` now measures **0.67 ms faster** than the 4× base, which
is the direction one would expect.

The decisive test designed in PERF-002 was run anyway, and it agrees: with the planar
reflection ablated, `WGR_MSAA=1` (10.43) and `WGR_MSAA=4` (10.38) differ by
**+0.05 ms — 0.5%, well inside the 4.1% spread of the arms themselves**, i.e.
indistinguishable. Whatever MSAA-specific cost the planar target carried is not
present in this build at this camera.

**Not claimed:** that anyone fixed it deliberately. No commit in this session touched
`ensure_planar_target`, so this is either a change landing since PERF-002 with an
unrecognised side effect, or the pathology was camera- or coverage-dependent all
along. The honest statement is that it does not reproduce, not that it was fixed.

## The grass arm measured nothing — do not use it

`WGR_ABLATE=grass` reads −0.22 ms, and that number is **vacuous**. The metrics JSON
reports `near_instances = 0`, `mid_instances = 0`, `far_instances = 0` against
`near_candidates = 270,400` — **in every arm, including base**. No grass was placed
at this camera, so ablating it removed nothing and the −0.22 ms is spread.

This is the §2.7 counter problem reproducing: instances came back 0 against candidate
counts in the hundreds of thousands. Whether the counter is lying or the placement
genuinely produced nothing is still undecided, and it now blocks a real grass cost.
That question has to be settled before any grass measurement means anything.

## The absolute is not comparable to PERF-002

Base here is **11.76 ms**; PERF-002's was 34.36 ms. **Do not read that as a 2.9×
improvement.** The camera is not confirmed to be the same one: PERF-002 was taken at
"the owner's Stratis camera", where water covered 6.50% and the planar regions summed
to roughly 27 ms (terrain 11.87, sky 8.71, clouds 6.45). Here the planar regions sum
to about 1.73 ms (terrain 0.78, clouds 0.73, sky 0.12, objects 0.08, mips 0.02), and
the water log reports `planar=1.686–2.037 ms` per frame.

A camera pointing at far less water explains that entirely, and so would a working
coverage gate. **Nothing in the log distinguishes them**: no coverage-gate line was
emitted at all, so it cannot be confirmed to have engaged. The interleaved deltas in
the table are valid; the absolute is valid only for this camera.

The remaining half of PERF-002's open question — whether the planar coverage gate
actually helps at the owner's camera — therefore stays open, and needs the owner's
exact camera to answer.

## What to do next

1. Settle the grass instance counter (§2.7) before attempting a grass cost.
2. Re-run this batch at the owner's own Stratis camera, and log the planar coverage
   fraction and chosen divisor so the gate's behaviour is observable rather than
   inferred.
3. Leave `ensure_planar_target` alone for now. Optimising a pathology that no longer
   reproduces risks fixing a measurement rather than a program.

## Process note

Two agent runs died mid-measurement — one on a transient `529 Overloaded`, one on a
stall watchdog — after the data had been written. The numbers above were recovered
from the harness's own JSON and logs rather than from either agent's report, which is
the reason to have the harness write artefacts to disk as it goes instead of
accumulating results in an agent's context.
