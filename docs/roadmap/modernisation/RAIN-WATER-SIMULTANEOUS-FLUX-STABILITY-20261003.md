# Fine water simultaneous flux stability

Source candidate based on `e017e028812dcd7ce050497fd40bf81253a1bcf1`.
Full game build, deployment, runtime cost and visual acceptance remain pending.
Fine water remains default off; this does not promote it.

## Evidence

Installed `bd590703` campaign
`build/rain-water-natural/actual-solar-native-owned-pool-20261003-133514-9e5936`
contains actual unchanged native Noe terrain, a selected owner and pool captures.
Near water has readable sky/tree reflections. The top capture has visible square
seams; it does not establish satisfactory water appearance.

The owner has 1,024 native cell-centres: 33 standing-water, 3 transition and 988
dry samples. Its 41 adjacent wet-centre pairs have mean absolute head difference
0.0394633223 m and maximum 0.0778808594 m. Of those, 35 faces are wholly below
both adjacent sampled heads at both native endpoints. The largest step joins
centres `(4896.875,9046.875)` and `(4903.125,9046.875)`, with heads 200.102585 and
200.180466 m; both heads exceed the higher shared native face endpoint by at
least 1.15759 m. Thus shoreline clipping alone does not explain these steps.
One snapshot cannot prove a temporal oscillation in that actual pool.

The old actual production solver does have a reproducible oscillation. On flat
fully wet 6.25 m native cells at alternating depth 2 +/- 0.04 m, each of four
edges proposes 0.78125 m3. Four simultaneous transfers flip the high and low
heads exactly, without violating its volume-only donor limiter. The new focused
regression, compiled against the unchanged baseline header, prints an interior
residual of 0.04 m and fails its damping assertion. With the candidate it prints
0 m and passes. This is an actual core update, not a duplicate flux formula.

## Bounded paired transfer

Keep the existing hydraulic/isolated-equilibrium proposal, native face profile,
donor wetness and signed paired transfer. Add two bounds at the arithmetic
midpoint M of each edge's donor and receiver heads:

```
donorLimit = max(0, donorVolume - nativeVolume(donor, M)) * edgeLength / incidentLength(donor)
receiverLimit = max(0, nativeVolume(receiver, M) - receiverVolume) * edgeLength / incidentLength(receiver)
amount = min(existingProposal, donorLimit, receiverLimit)
```

`nativeVolume` is the existing exact clipped-triangle V(H) for fine cells and the
existing flat volume law for coarse cells within the fine solver. It is not a
full-area derivative estimate on a partially wet triangle. Incident lengths are
computed once from the actual rebuilt edge graph. A coarse face split into four
fine subfaces contributes all four short lengths; no fixed degree of four is
assumed. The clipped coarse-corner regression has eight incident fine edges.

For each cell the edge-length weights sum to one. Every outgoing edge has a
midpoint at least `(ownHead + minimumIncidentHead)/2`; monotonic V(H) therefore
bounds the sum of all proposed withdrawals by the volume between the own head
and that lower midpoint. The corresponding incoming bound follows from the
maximum incident head. Simultaneous net transfer consequently cannot move the
cell outside these midpoint head bounds, apart from the existing inverse-head
floating-point error. The unchanged final donor-volume scale can only reduce a
transfer and preserves the same bounds. Both endpoints receive exactly the same
signed flux, so this adds no source or sink.

Rain, infiltration, evaporation, sea outlets, paused/deferred source behavior,
source geometry admission, ownership, reconstruction and renderer publication
remain in their existing paths. In particular, no exported-head average,
renderer height displacement, bed replacement or roof/source admission is
introduced. The legacy `RainWaterField` solver is unchanged.

## Checks executed

Using Clang C++20 with `-Wall -Wextra -Werror`:

- New actual baseline regression fails with 0.04 m residual as described above.
- `Test-RainWaterFine.ps1`: existing ownership/flow/geometry tests and new actual
  stability tests pass. New tests check damping, local midpoint maximum bounds,
  constant-head lakes over fully and partially wet native triangles, dry shared
  rim blocking, eight-edge unequal-area topology, every subface exchanging in
  both directions, mass ledgers, sea sinks, partitions and deferred/invalid
  source geometry. Near-zero balanced decimal rain can retain ~1e-20 m3 in an
  empty sloped triangle; its dry test checks mass instead of demanding an exactly
  unchanged ill-conditioned inverse head at zero.
- `Test-RainWaterField.ps1`: legacy physical field and source bridge tests pass,
  including queued genuine edits, stale closure, exact corners/query, overflow,
  source lifetime and mass-preserving regrids.
- `Test-RainWaterNatural.ps1`: pure production selection/reconstruction,
  dry-only retirement, saturation, source staleness, pause and mass tests pass.
  Retail/native corpus replay was not executed for this candidate.
- `git diff --check` passes.

## Required runtime review

The limiter adds two analytic volume evaluations per nonzero proposed edge and
one cached incident-length double per allocated cell; vector capacity is included
in `StorageBytes`. No runtime timing benefit is claimed. Review measured fine
Advance cost and normal-exit installed behavior before promotion, especially
moving-interest topology and partially wet slopes.

Repeat actual native pool head/face queries and the same near/top captures after
genuine rain. Check whether alternating interior head steps settle without
false dry gaps, floating fragments, mass loss or slower drainage that impairs the
intended water behavior. The earlier sunny deep-focus sample gained about 3 cm
while receiving incoming water; growth alone is not evidence of this grid mode.
Puddle reflection, colour and final appearance remain a separate visual gate.
