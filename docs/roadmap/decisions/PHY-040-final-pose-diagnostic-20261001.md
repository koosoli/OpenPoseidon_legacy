# PHY-040 — Actual final corpse pose diagnostic

Source candidate based on `aa33a726` plus the authored-component measurement.
Build and installed acceptance are pending. No articulated bodies, pose setter,
root movement or default death replacement are enabled.

## Actual consumers

`Man::PrepareCorrectedPose` now contains the existing production head, aiming
and graphical-leg correction block, extracted without changing its arithmetic
or order. Ordinary `Man::Animate` consumes that helper before the existing face
and matrix application. This supplies the diagnostic with the actual corrected
palette rather than a second mathematical approximation.

`Head::EvaluateDeadFaceBones` uses the same arithmetic as `ApplyFaceBones` through
its shared implementation. It refuses nondead evaluation and suppresses head
state/counter writes. It does not copy `Head`: the legacy `SRef` copy constructor
transfers the lip pointer, so copying a head would break read-only behavior.
Dead evaluation does not sample lip phase or alter lip ownership. Ordinary
rendering still uses the recording path. Missing/out-of-range face bindings
return false; no synthetic parent or offset is manufactured.

The new harness command `corpse_pose_state` requires `--dev`, local single player
and an already valid settled dry stock-WB hold. It returns newline-terminated
`HarnessProtocol::JsonResponse`. First capture through the existing command:

```sqf
triPhysicsShowcase "corpse-capture"
```

Then send the harness JSON request `{"cmd":"corpse_pose_state"}` using the
existing TCP client. Use the actual harness request envelope where its client
requires additional request fields. The getter has no action/pose arguments.
An expired, deleted, hidden, foreign-world/model/move or unsupported-model hold
refuses. Clearing remains `triPhysicsShowcase "corpse-clear"`.

## Measurements and honest limits

The response contains the actual entity/model, hold and measurement simulation
times; all runtime bone names; head/gun identity flags and numerical identity
errors; actual head/gun/leg correction matrices; and every stock runtime LOD's
complete palette, including the evaluated dead synthetic face tail where
bindings exist. Matrices are row-major 3x4 model-space affine data. Singular
matrices remain measurements (`minAbsDeterminant`), never inverted or silently
admitted. The exact asset name is a bounded diagnostic gate, not a runtime hash
verification of installed data; the prior source asset hashes remain required
provenance.

Each LOD reports point count, unweighted count, point comparisons, skipped
synthetic phase tails, maximum discrepancy between actual `AnimatePoint` and
`ApplyMatricesPoint` on the final palette, and palette-derived point bounds.
`pointOnly=true` identifies the legacy one-point path, which directly uses
`AnimatePoint` rather than the matrix palette. The bounds and hypothetical
palette comparison do not pretend to replace that path. Proxy matrices are
captured through actual `AnimateProxyMatrix`, including existing
orthogonalization/correction semantics. This diagnostic does not mutate mesh
positions, step simulation or move an entity; ordinary original-position/query
caches may initialize as in existing point queries.

All fourteen actual Fire Geometry components report normalized runtime
single-bone ownership, posed world bounds, lowest actual roadway/terrain support
clearance, and a corpse-ignored short center ray against actual Geometry.
Those are useful failure witnesses, **not complete convex contact admission**:
an object can intersect a hull away from its center ray, and vertex support is
not a hull/terrain manifold. `contactAdmitted` and `rigAdmitted` are always false.
The twelve anatomical components and two equipment components remain explicit.

Nine candidate anchors are recomputed from actual recentered graphical source
selection overlap, excluding proxy helpers. The diagnostic reports overlap
count, weighted source centroid and each adjacent final-bone world endpoint,
including their separation. Zero-overlap candidates remain count-zero evidence,
not a fabricated origin anchor. Endpoints are not forced to coincide; no hinge
axis, cone limit, body mass or anchor acceptance threshold is invented.

## Next moving-pose proof, before a solver

The getter is intended to answer whether this actual settled WB pose has
identity post-corrections, complete face binding, usable hull ownership and
coincident candidate anchors. The source-proven mixed-weight discrepancy still
prevents assuming those answers.

The next bounded change should retain the same stock, local, dry, one-corpse
lease and apply a **common rigid model-space delta** after final consumer
operations. It is a complete-consumer foundation test, not a one-body ragdoll.
Identity must follow the original arithmetic exactly; nonidentity must affect
graphical palettes, special/memory points, one-point contact LODs, actual proxy
matrices and the final face tail once, with posed bounds/shadow invalidation.
`AnimateMatrix(weight)` and its selection overload require separate final-stage
handling so nested calls do not apply the delta twice. The face palette path
must not double-transform CPU face fallback offsets. A model lacking actual
complete tail binding must refuse that proof instead of advertising completion.

Production tests should exercise translation and rotation of nonrigid,
multiweight, unweighted and synthetic-face consumers; identity equivalence;
finite/matrix-range rejection; and the existing model/world/move/lifetime
retirement paths. Then installed screenshots and actual point/proxy comparisons
must establish common-delta correctness on the real corpse. Independent limb
deltas need preserved per-consumer affine residuals or demonstrated actual
identity corrections, so their zero-motion limit cannot jump to different
legacy arithmetic. Only after those gates should the ten-body authored-hull,
axis-free spherical-joint prototype take ownership of simulation.

## Validation status

An exact source comparison confirms the extracted production correction block
is unchanged. `git diff --check` passes. New Catch2 cases use actual production
face evaluation and actual animation `CombineTransform`, `TransformPoint` and
`ApplyMatricesPoint`; they check dead-face output equivalence, lip-pointer/state
and counter retention, refusal bounds, nonrigid determinant/identity metrics,
and the real mixed-weight correction discrepancy. The initial source handoff
required parent-owned compilation and installed regression checks; their
subsequent results are recorded below. No GPU, game launch, full build or large
stock animation corpus was run by this agent.

## Installed runner candidate

`scripts/Test-CorpsePoseDiagnostic.ps1` wraps the proven no-motion installed
runner through its optional `FinalPoseProbe` callback. Without that callback,
the original proof's gameplay/lifecycle behavior is unchanged. The wrapper
uses the same serial game lock, installed pair hashes/stamp, private profile,
real stock spawn/kill, actual-corpse camera, bounded capture wait and owned
normal exit. Additional snow depth/rate/flakes/snowline controls are restored
after the campaign and requested values are retained in provenance.

The callback checks the getter while the initial default has no hold, twice
under the same paused valid hold, after explicit clear, after actual >30s
expiry, before and after hiding, on a second stock corpse and after deletion.
The exact absent-hold error must be `no-valid-hold`; an unknown command or
missing implementation fails. Raw replies remain in `harness.jsonl`, with
successful measurements copied in `result.json`. Source line/stack are
retained on failure. The getter's repeated paused JSON and before/held/cleared
PNG hashes must be identical; timestamps must remain paused. The normal
authored-death screenshot is retained as `before-held.png`. No delta setter
is available or invoked.

Typed finite matrices, correction metrics, complete 25+8 bone coverage, all
contiguous LODs and required special consumers, actual synthetic face binding,
proxy matrices, the fourteen measured component mappings and nine paired anchor
records are required. Mixed-weight discrepancy, anchor gaps and support
penetration remain recorded evidence; passing this runner still leaves
`rigAdmitted=false`, `contactAdmitted=false` and `solver=false`.

Source-only validation:

```powershell
pwsh -NoProfile -File scripts/Test-CorpsePoseDiagnostic.ps1 -SelfTest
```

The helper test accepts a complete typed mock receipt, then rejects changed
owner/model, rig admission, nonfinite matrix, lost point coverage, incorrect
hull mapping, absent face evaluation and expired lease. It also rejects unknown
and falsely successful refusal replies. These are parser tests, not fabricated
runtime evidence. Actual installed execution remains pending root ownership:

```powershell
$env:LOCK_OWNER='corpse-final-pose'
bash scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-CorpsePoseDiagnostic.ps1
```

## First installed diagnostic and exact baseline readiness

The parent-owned combined build passed, including `[corpse-pose]` (257
assertions in six cases). Installed `e5445111` ran the diagnostic in
`build/corpse-pose/final-pose-installed-20261001-052726-193320`. It failed the
exact image gate and exited normally; this record remains a failure, not an
accepted complete lifecycle proof.

At simulation time 17.374, the repeated held getter replies were identical.
The stock corpse reported 33 final matrices, identity head/gun/leg corrections,
and 37 proxies. Existing phase-consumer comparison covered 542 points with
zero mismatches. Actual graphical final-palette versus point-path discrepancies
were at most about 6.4e-8 m; special-LOD comparisons were zero. The before/held
PNGs differed in exactly fifteen scattered pixels, each by one unit in one RGB
channel, with none on the corpse. Held and cleared PNGs were identical.
The nine measured anchor separations reached 0.078774 m; anatomical hull
clearance reached -0.07466 m and the equipment tube -0.22952 m. These remain
observations with contact and rig admission false.

Source inspection found that `EngineWgpu::UpdateSunGlareExposure` continues
its render-frame adaptation while simulation is paused. It multiplies manual
tonemap exposure independently of `WGR_AUTO_EXPOSURE=0`. This is a plausible
readiness cause, **not a measured attribution of the failed run**. Tonemap and
GTAO noise have no frame-varying term, and the screenshot path drains a real
next-frame composited readback.

The diagnostic callback arm now captures up to eight paused baseline PNGs
before any hold, comparison or final-pose query. It retains every image and
requires three consecutive exact hashes at the same simulation time. If that
never occurs, it fails explicitly at baseline readiness. The final selected
baseline is copied byte-for-byte to `before-held.png`; the existing exact
before/held/cleared gate is unchanged. The ordinary proof without a callback
keeps its previous capture behavior. This can establish query-independent
drift on a repeat without accepting a pixel tolerance. Root runtime must
still verify the candidate; persistent drift requires a separate measured
exposure/readiness trace, not a relaxed equality check.

The source-only wrapper test executes the actual host baseline predicate and
checks empty/two-frame refusal, an eight-frame drifting sequence, three exact
final matches after drift, rejection of a later changed hash and rejection of
simulation advancement. No game or GPU was run by this agent.
