# PHY-040: measured stock elbow and knee hinges

Source baseline: `afffcc53`, installed spherical prototype already accepted in
the bounded `442e46` campaign. This candidate adds a **separate developer opt-in**
`triPhysicsShowcase "corpse-articulate-hinges"`. Normal deaths and the existing
`"corpse-articulate"` spherical control keep their current behavior. Source/native
checks below do not establish installed appearance or broad contact acceptance.

## Actual geometry and reference

The stock graphical model has shared wrist (`ploket/pruka`, `lloket/lruka`), ankle
(`pholen/pchodidlo`, `lholen/lchodidlo`) and toe (`pchodidlo/pprsty`,
`lchodidlo/lprsty`) boundaries. The read-only ODOL7 census finds 12, 13 and 13
positive shared vertices on each side respectively, excluding proxy helpers.
Runtime admission measures those boundaries again with the original engine
positions and actual source selection weights. The existing nine-anchor
read-only getter and spherical control remain unchanged; only the new opt-in
requests the six additional endpoints.

Neutral lateral comes from bilateral shoulder and hip centroids. Neck-to-hips
defines the longitudinal direction; projected lateral and longitudinal define
the body's plane. Actual ankle-to-toe geometry supplies the forward sign. Missing
or degenerate geometry is refused. There is no fixed world/model hinge axis,
joint inferred from an RTM translation, or assumed stock file bone ordering.

For each elbow/knee, the actual authored proximal affine transforms its neutral
proximal-to-joint tangent; the actual distal affine transforms joint-to-wrist or
ankle. Their cross product gives a bent limb's plane. The proximal affine also
carries neutral forward, establishing forward elbow flexion and backward knee
flexion. A nearly straight limb uses that measured reference plane instead of an
unstable tangent cross product. Singular/nonfinite transforms, short segments,
ambiguous near-folded planes and bend/reference agreement below 0.7 are refused.
The helper reads the affine matrices without modifying scale/shear or skinning.

Read-only actual-palette evidence uses
`final-pose-installed-20261001-052726-193320/result.json`,
`gates.finalPose.activeHold.actual.levels[0].palette`, SHA-256
`408ebd202230dd368b699fd4abe62a37f7f501a70a7acd059fbba49e1be63737`.
The decoded `mc vojakw2.p3d` SHA-256 is
`baf90043df1b561c6099f3c8fec546ed58e43e00853d70f386cfb2f81853d221`.
The engine flips raw ODOL X/Z before recentering; all nine actual captured
boundary centroids establish that conversion. Applying raw ODOL axes directly
to the runtime palette would reverse the apparent bends. Production reads engine
`OrigPos` directly, and the native frozen fixture applies the evidenced conversion
to endpoint differences.

| Captured limb | Signed bend | Plane/reference agreement |
|---|---:|---:|
| Right elbow, `ploket` | 54.8879 degrees | 0.99960 |
| Left elbow, `lloket` | 48.7506 degrees | 0.99967 |
| Right knee, `pholen` | 51.6487 degrees | 0.97176 |
| Left knee, `lholen` | 66.7816 degrees | 0.99518 |

These are measurements of that historical captured pose, not a claim that all
death clips or the next installed capture have those angles. The new runtime
`CORPSEHINGE` log records each actual neutral endpoint, transformed tangent,
reference, axis, signed angle, selection count, paired endpoint separation,
limits, admission and exact refusal stage before creating any bodies.

## Solver mapping without a handoff correction

The four distal joints become true Box3D revolute joints; the other six remain
spherical. The hinge's local Z is the measured world bend-plane normal. Both
initial world joint frames coincide at the existing common paired-anchor
midpoint. Body origins, initial collider calibration, every authored phase and
all palette/point/proxy consumers remain unchanged.

Experimental anatomical limits are -5 to 150 degrees for elbows and -5 to 145
degrees for knees. These are an explicit bounded policy, not measured human
biomechanics. Captured bends outside the policy are refused. Because coincident
initial solver frames read relative angle zero, each limit subtracts the measured
captured bend. Zero therefore lies inside the initial limits; no motor or spring
pulls the body toward an extension pose.

The pinned official implementation makes this distinction explicit:
[relative twist getter and signed limits](https://github.com/erincatto/box3d/blob/954cf879e717334eb96da9a5c255788fdc9f5f87/src/revolute_joint.c)
use actual frame twist; `targetAngle` is used by the optional spring. It is not a
limit offset. The engine seam validates rigid local frames, signed finite ordered
limits within the upstream range and known joint kinds, retaining the existing
generation-checked ownership and joint-before-body cleanup.

The dry local-SP western-stock-only, complete palette, real every-vertex 3 mm
clearance, 25 mm terrain/provider agreement, external-contact rejection,
0.5 m travel, 30-second lease and retirement gates are retained. This stage does
not repair authored graphical burial, admit building floors, extend the lease,
freeze a retiring pose, support water, or enable automatic deaths.

## Required checks

The private focused executable compiles the candidate backend/facade, production
animation and affected existing native tests, linking unchanged root support
libraries and the accepted pinned Box3D package. It passes **2,302 assertions in
four hinge cases**, then **3,952 assertions in 50 selected physics/corpse/articulation
cases**, including the registered actual mass/contact regressions. SoldierOld
Clang syntax and the runner's PowerShell parser pass. The four new cases verify:

- Actual captured stock affine coefficients and geometric endpoints, signed
  bends, axis covariance under whole-world rotation, reverse-flexion refusal and
  unmodified original affine matrices.
- Nearly straight nonrigid reference behavior and refusal of ambiguous, singular,
  degenerate or nonfinite input.
- Actual off-center impulses through the engine seam: hinge-axis flexion in both
  directions, blocked perpendicular bending, real signed limits, paired pivots,
  180-frame solver record/replay and zero-handle cleanup.
- Invalid kinds, frames and limits refusing without allocating joints.

Parent's committed native target should run
`PoseidonTests.exe "[corpse-hinge]"`, followed by the existing relevant selected
`"[physics],[articulation],[corpse-pose]"` checks. Clang syntax and PowerShell parser
checks precede handoff. The actual installed campaign, run only by root under its
game lock, is:

```powershell
scripts/Test-CorpsePoseProof.ps1 -ArticulationSmoke -AnatomicalHinges -Label measured-stock-hinges
```

The new switch requires the complete existing articulation campaign and requires
the actual response `hinges=4`. It preserves identity, exact zero-motion
33-palette/point/proxy equivalence, genuine solver-driven motion, terrain checks,
clear/expiry/hide/delete cleanup and stable installed-pair provenance. Installed
visual acceptance must still judge skin, weapon, shadows, near/far culling and
elbow/knee motion before promoting even this bounded new opt-in.

For the previously accepted spherical control and its limits, see
[the vertical slice](PHY-040-stock-corpse-box3d-vertical-slice-20261003.md).
