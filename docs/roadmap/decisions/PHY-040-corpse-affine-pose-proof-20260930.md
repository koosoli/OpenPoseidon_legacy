# PHY-040: bounded shared affine corpse pose proof

This candidate implements the next prerequisite after the stock rig census: an
explicitly captured, corpse-owned authored pose consumed by the existing rendering,
special-LOD palette, Memory point and proxy matrix paths. It does not add Box3D
bodies/joints, a solver, root motion, a new death animation or default activation.
The stock death behavior remains the ordinary path.

## Actual shared consumers

`Man::Animate` prepares the base palette through `PrepareBasePose`, before the
existing head/aiming/leg corrections, facial bones, `ApplyMatrices` and
`BasicAnimation`. Consequently graphical CPU/GPU skinning and Geometry/Fire/View
LOD animation receive the same held base pose. The one-point LOD path still uses
`AnimatePoint`. `Man::AnimatePoint` uses the held phase pair before its existing
aiming/leg corrections; `Man::AnimateMatrix` uses it before the existing final
orthogonalization and selection head/aiming corrections. `AnimateProxyMatrix`
continues through that matrix consumer. Explicit `ApplyAnimation` clears the hold
and executes the requested authored pose.

The copied data are the actual loaded previous/next phase arrays and the original
interpolation factor. This is necessary for exact arithmetic: `PrepareMatrices`
first multiplies phase matrices by the animation blend factor, whereas `Point`
and `Matrix` first multiply each phase matrix by the vertex weight. Preblending
one palette and then multiplying weights changes floating-point operation order.
The capture retains arbitrary finite affine matrices, including stock scale and
shear, rather than converting them to quaternions or orthogonalizing them.
`AnimationRT`'s ordinary three consumers are unchanged. Its new capture method
uses the same phase lookup, lock, cache and interpolation arithmetic.

## Admission and lifetime

The explicit diagnostic captures only the nearest eligible corpse within 50 m of
the current camera. It requires local single player (`GModeArcade`), the reached
dead animation and the existing five-second `ShadowPoseFrozen` condition, dry
ground contact, no hiding, ladder, hierarchy parent, deletion or landscape exit.
Both contributing animations must supply a finite phase pair on the actual
weight-table skeleton, with 1..128 skeleton bones and all weight indices in range.
Every Memory and one-point LOD vertex must be representable by each contributing
phase pair. A successful capture replaces the previous hold; a refusal preserves
the previous hold.

The maximum is one held corpse, for 30 simulation seconds. The weak global link
stores only owner identity; phase copies and strong model/skeleton/weights/animation
references belong to that Man. The strong references prevent pointer reuse from
admitting unrelated data. Model, skeleton, bone count, weight table, animation
identity, primary/secondary move or blend factor changes invalidate the lease.
Clock rewind, expiry, leaving single player, revival, movement that invalidates
the settled-death condition, ground/water/attachment/hide changes and removal
also invalidate it. `Simulate` and each actual consumer validate; `HideBody`,
serialization, explicit authored `ApplyAnimation` and destruction clear it.
Clearing releases copied matrix allocations and retained resources. Nothing is
added to save/network formats.

Graphical synthetic face bones differ from Memory point weights. The existing
palette preparation fills the skeleton tail with identity and the existing
weighted matrix consumer falls back to its input matrix for that tail. Both
behaviors are preserved. Legacy `Point` has no tail fallback. The in-tree Man
point queries use Memory LOD or a one-point LOD; admission covers all those
vertices. An unexpected later Point query outside a captured phase range clears
the entire hold before returning to the legacy path. A later unsupported matrix
weight does the same. This avoids adding an invented facial-point interpretation
or leaving a partly active held corpse.

## Executable validation

The native Catch2 cases in `test_corpse_pose.cpp` generate a two-phase nonrigid
RTM and load it through the production `AnimationRT` constructor. They compare
actual production `PrepareMatrices`, `Point` and `Matrix` results with captured
results, including primary/secondary accumulation, threshold factors, endpoint
times, quantized multiweight points, unweighted CURRENT position fallback and
synthetic matrix-tail fallback. Palette/matrix comparisons are bitwise. Tests
also check owned-copy independence, invalid/nonfinite recapture, empty animation,
bone cap, stale model/world/skeleton/move/factor, expiry and time rewind. The native
test command after the parent builds its configured target is:

```powershell
build/win-x64-clang-rwdi/tests/unit/engine/Poseidon/PoseidonTests.exe "[corpse-pose]"
```

The existing test evaluator command accepts these additional strings:

```sqf
triPhysicsShowcase "corpse-capture"
triPhysicsShowcase "corpse-status"
triPhysicsShowcase "corpse-compare"
triPhysicsShowcase "corpse-clear"
```

The Physics developer panel exposes the same operations. Capture/status return
actual owner address, model, bone/phase matrix counts and lifetime. Refusal returns
the actual eligibility or phase/point-range gate. Clear reports whether a lease
existed. Compare runs the actual Man base palette, point and proxy functions both
with and without its captured pose, in one command without advancing simulation
or animating shared mesh state. It compares every base palette matrix, up to 64
evenly spaced supported points per loaded LOD, and every valid nonempty proxy
selection; it reports actual tested counts, skipped synthetic point tails,
mismatches, maximum point error and remaining hold validity. Sampling is explicit:
this is not a claim that every graphical vertex or every final GPU pixel was
tested. Existing facial/head/aiming/leg code remains common to both paths.

Installed acceptance belongs to the parent agent: build game and these tests,
deploy the matching stamped binaries, create a stock corpse away from water,
wait for the authored death pose to settle, capture/compare, inspect installed
views, clear, recapture/expire, hide/delete/save and verify fallback. Candidate
source review and diff checks do not substitute for that installed run.

Root completed the committed `77299a74` PoseidonGame/PoseidonTests build and
deployed the matched EXE/DLL via the locked deployment script. Native
`[corpse-pose]` tests pass **228 assertions in four cases**, with no skips.
The rebuilt game passes normal-exit grass startup at Oliver's exact Everon
camera. Explicit stock corpse capture and lifetime acceptance remain pending
the isolated installed harness; this is not a completed ragdoll feature.

## Next actual articulation step

After exact shared-pose equivalence is accepted, retain this same owned affine
phase data and apply identity-at-capture rigid deltas consistently to all three
consumer paths. A nonidentity delta needs separate multiweight and proxy tests;
it must preserve the original phase/weight arithmetic, not quaternionize the
authored palette. Joint frames, meaningful group envelopes (the census's large
pelvis envelopes are not admitted colliders), simulation ownership, ground/water
handoff and default-safe cross-generation/save/network behavior remain gates for
an opt-in articulated stock corpse. This proof deliberately creates no dormant
physics rig whose admission has already failed.
