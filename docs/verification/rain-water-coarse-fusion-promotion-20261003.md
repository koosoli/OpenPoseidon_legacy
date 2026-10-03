# Guarded coarse flux promotion, 2026-10-03

The separately reviewed offline prototype showed a 16.47% rainy-state solver
wall-time reduction in a quiet four-round ABBA run, with bitwise equal
observable state. Dry timing was treated as noise. The actual captures,
finite proof and raw receipt are documented in
`rain-water-coarse-fusion-prototype-20261003.md`. This production candidate
implements that same bounded single-pass dispatch; it does not claim an
installed game frame-rate improvement.

The guard checks all coarse beds, including sea cells before their source
loop continues. Spacing 10..1e6, bed absolute height <=1e6, and post-source
zero-or-normal finite depth<=1024 exclude overflow and unsafe donor bounds.
Outside those conditions, the existing cached transfer and donor limiter
remain. The empty skip, rain/source/sink operations, signed edge/delta/flow
order, pending/backlog, budgets and revision remain. No per-Step allocation,
copy-only benchmark bridge, restored state or fast-math enters the game path.

Diagnostics append FusedFlux at index 5; historical indices 0..4 remain intact.
Combined raw transfer/delta/flow work is named `fused-flux`, never labelled
as the old raw-transfer or limited-flux phase. World emits `phase_count=6`;
all six phase rows, including zero-valued ones, are required. The strict
runner parser still accepts historical five-row reports without that field.
It rejects unknown/duplicate/truncated schemas, and requires raw==limited
and raw+fused==steps-empty, with each common phase equal to the step count.

The faithful baseline runner remains strict about matching captured source.
The separate fusion proof's explicit `-Production` mode copies the full
current header by renaming/friend/witness insertion only, verifies the reverse
round trip, records its exact source/dependency hashes, and compares it with
the frozen captured baseline. Thus the original capture's source identity is
not silently relabelled as the newer production revision.

CPU/source checks:

- Actual captured Dry/Rain observable state matches bitwise after fourteen
  Advances at Clang O0/O2, with the synthetic dispatch/overflow/source-transition
  and extreme admitted four-face controls.
- Existing pinned actual coarse reference matches for terrain/sea/finite
  extremes, rain/drying/wind, edits/regrid/backlog/partitions, MaxCells and
  zero per-Step allocations, under both enabled and disabled cost diagnostics.
- Cost unit tests preserve old indices and distinguish actual cached spacing 2
  from fused spacing 25 calls.
- Runner self-tests cover legacy, mixed, fully fused and zero-fused reports,
  mandatory sixth-row truncation, phase order, counts, duplicates and malformed
  fields.

Root owns the full combined game build, matched deployment and installed
functional/quiet flight checks. This source checkpoint remains uninstalled
until those complete; the prior offline gain is not a claim that the owner's
executable already contains this change.
