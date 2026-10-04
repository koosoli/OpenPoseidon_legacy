# Original OFP character ragdoll capability

Source candidate based on `3427b5e2`, 2026-10-04. Installed qualification is
pending; this document does not claim that all models have passed gameplay.

The three Western filename checks prevented Eastern infantry, officers, pilots,
crews and civilians from reaching the existing Box3D admission. Capture,
diagnostic inspection, articulation and frozen-load now share exact original
stock filename and live rig capability checks. The census has 37 present
class-backed assets (35 public models plus story-only Angelina and GeneralE),
using CfgMovesMC or CfgMovesMCWoman. The exact list additionally retains the
unreferenced original `mc civil2stary.p3d` asset. The absent template
`data3d\vojakw.p3d` and unknown addon names remain unsupported.

Source census: [actual installed character inventory](../../verification/ofp-ragdoll-model-census-20261004.json).
No retail model is copied into the repository. Filename membership alone never
admits a body: the actual model must supply all 25 unique named stock body and
equipment bones, followed by exactly the eight implemented synthetic face
bones. Different body bone orderings are supported by name; duplicate, unknown,
misplaced or missing names are refused. Existing special LOD, point, palette,
proxy, zero-motion consumer, affine bounds, terrain, water, world and solver
checks remain in force. Geometry comes from each owning model.

The 22 class-backed assets with all 15 weighted graphical boundaries retain four
measured elbow/knee hinges. Fourteen civilian/pilot/female assets have missing
toe and sometimes ankle boundaries. They use the existing bounded spherical
11-body/10-joint control, including its existing cone/twist constraints. Their
nine physical joint boundary checks still require real shared weighted skin
vertices and at most 0.12 m captured separation. Missing direction evidence is
not presented as a measured hinge or reconstructed from Western anatomy.

Angelina also lacks five physical shared skin boundaries across every graphical
LOD. For this exact original asset only, the missing boundaries use clustered
nearest pairs in her own exclusive single-bone authored FireGeometry parts.
The source and captured gaps must both be at most 0.12 m; tied pairs within 5 mm
are averaged, and clusters dispersed more than 0.25 m around either centroid
are refused. The actual FireLOD consumer supplies each world point. Diagnostics
record `authored-fire-boundary-pair`, source LOD and pair count, with
`sharedVertices=0`; they do not claim graphical overlap or anatomical hinge
evidence. Angelina consequently uses the same spherical control.

Private checks execute the actual production helper and registered native test
definitions: three cases, 1,556 assertions pass, including exact model names,
all rig name ranges and permutations, actual Angelina chest/head source pair,
captured-gap refusal, tied-pair order independence, dispersed clusters, huge
finite inputs and missing/nonfinite data. SoldierOld.cpp and the full registered
test translation unit also pass source syntax checks. Root must build the
ordinary native target and run each model's installed capture/fall/freeze,
native projectile reaction and cleanup before appearance acceptance.

Model coverage does not increase the four simultaneously simulated corpses or
64 retained owned poses. A death that exhausts these budgets keeps its authored
behavior and has no current wake recipe. Fresh runtime frozen owners can wake
on actual incoming hits when a slot is free. All currently saved/restored frozen
poses also lack a wake recipe; save-load preserves their visible pose with zero
handles, but projectile reactivation after load remains a separate task.

The existing impact safety policy may restore an excessive local impulse trial
and distribute the bounded aggregate impulse across the rig. A verified native
hit, actual pose change and safe resettlement establish a reaction; a local-only
transfer is not a requirement. The 20 rad/s solver guard is unchanged.

## Articulated energy dissipation and capture diagnosis

The first installed 37-model campaign did not qualify all models. GeneralE
and Civilian8 retained real head/arm angular motion of roughly 2–6 rad/s after
60 simulation seconds, above both the quiet retirement and final freeze gates.
The 2026-10-04 settlement candidate applies Box3D angular damping of 1.0 per
second only when creating articulated pieces. The pinned upstream body default
is zero. Initial angular/linear velocity and immediate impulse response are
unchanged, and no linear damping is added. Ordinary dynamic pieces retain their
previous behavior. Ground support, 20 rad/s safety and retirement predicates
remain unchanged; installed resettlement must establish the correction.

An actual changed-backend native control passes 28 assertions: identical
initial motion, preserved immediate center impulse, angular decay, ordinary
undamped control and matching translation/gravity over two seconds. Capability
controls still pass 1,556 assertions in three cases. Source syntax checks pass.
These are source/native evidence, not installed model qualification.

Women passed the detailed initial transform gate but failed a later capture
stage. The candidate identifies each later phase, point/palette, Memory weight
or proxy-weight refusal with actual model/LOD/index/count witnesses. No asset
or consumer gate is weakened pending that installed diagnosis.

## Same-part stock equipment proxy blends

The installed settlement retry qualified GeneralE and Civilian8 through death,
31-second frozen persistence, real projectile wake/resettlement, frozen save/load
and cleanup. Woman1's exact later refusal was its LOD0 RPG proxy: three finite
native bone influences, rather than one. The source census independently shows
that all five female variants' graphical RPG helpers blend hrudnik, lrameno and
roura, which all belong to the existing chest solver part.

The follow-up admits only finite positive normalized proxy weights whose
contributing body/equipment bones map to exactly one existing solver part.
It retains the actual captured AnimateProxyMatrix byte-for-byte at handoff,
then applies that part's common rigid delta. Capture, automatic inspection and
frozen-load validation share the same rule; articulation creation additionally
checks it against the actual constructed bone-to-part mapping. Cross-part,
unmapped, out-of-range, nonfinite or nonnormalized blends still fail admission.
No collision, support, safety, budget or save-loaded wake policy changes.

The actual production helper plus owned corrected-proxy consumer tests pass
1,583 assertions in five cases, including finite/weight/mapping refusals,
zero-motion byte equivalence, common-delta/native-blend equivalence and repeated
updates against the retained capture. The source translation units pass syntax
checks. Installed female qualification and a final same-build 37-model campaign
are still required before claiming complete stock model coverage.

## Sparse spherical root midpoint direction

The next installed build qualified Woman3/4/5, including real native projectile
reaction and frozen save/load. Woman1/2 both passed exact capture (396 matrices,
6,191 or 5,882 points, 44 proxies), then refused only spherical joint0. The
actual pelvis/chest centre separation was 30.61 mm, while the generated midpoint
left a 15.31 mm half-vector below the 25 mm direction threshold.

Only that spherical root joint now measures its complete actual parent-to-child
centre direction, preserving both body origins and the generated midpoint.
The helper verifies the supplied midpoint within 1 mm and retains finite and
at-least-25-mm centre-separation admission. Every other joint and measured hinge
keeps its existing axis policy. No general alternate-pivot fallback is added.
Native controls include the actual captured Woman1/2 centres, rotated frames,
coincident/24-mm centres, incorrect midpoint, nonfinite and overflow inputs.
Capability/consumer controls pass 1,598 assertions in six cases. Final installed
Woman1/2 and same-build all-model qualification remain pending.
