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
