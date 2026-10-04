# Original OFP character ragdoll coverage

Oliver explicitly requested ragdolls for all original OFP characters on
2026-10-04. This is a finite model-coverage extension of the default-on Box3D
ragdoll implementation; the existing physics toggle remains shared with the
Physics dev panel.

## Coverage and admission

The local retail configuration census identifies 37 present character models:
35 with editor-public classes and two story character models. The missing
scope-zero `data3d/vojakw.p3d` template is not a playable installed character.
The complete census, inherited model/movement bindings, and original asset
presence are recorded separately in the model census.

Admission must establish the model's actual skeleton, weighted consumers,
anatomical geometry and native/solver ground contact. It must not substitute
Western soldier dimensions for a different model. Original civilian and story
meshes omit some graphical landmarks, so a model-name whitelist alone is not
sufficient evidence of support.

Implementation source `f74ece43` replaces the Western-only checks with the
shared exact stock model and live rig capability checks. Twenty-two class-backed
models retain the four measured elbow/knee hinges. Fifteen sparse models use
the existing constrained spherical joints; Angelina's missing skin boundaries
use finite, clustered pairs from her own authored FireGeometry. This is
recorded separately from shared graphical-vertex evidence.

The normal native target passed 16,036 assertions in 79 cases with filter
`[physics],[articulation],[corpse-pose]`. The supported paired deployment stamped
`f74ece43` at 15:20 local time. Later runner corrections do not alter engine or
renderer source.

## Runtime acceptance

Installed qualification in progress. Required witnesses for each model
are an ordinary death, an actual eleven-body/ten-joint Box3D articulation,
finite retained animation/point/proxy consumers, supported ground settlement,
and a real projectile that changes the retained corpse pose and resettles.
Subset runs are diagnostic and do not alone establish the complete denominator.
The original full attempt was normally closed after reproducing a PowerShell
array-arithmetic error before native projectile creation. Its partial receipts
remain under `build/stock-ragdoll-models/full-stock-37-20261004-152030`.
The second full attempt uses corrected scalar projectile construction. A
subsequent stale-count observer failure was repaired by resampling ownership
after pausing simulation; affected models require the complete sequence again.

## Existing limits

The single-player local-ownership, water/attachment/removal, finite-motion and
physics allocation limits remain in force. Four simultaneous articulations
and 64 retained corpses are performance limits independent of model support.
Budget fallback uses authored death animation. Existing frozen runtime poses
retain their validated wake recipes. All currently saved/restored frozen
corpses and authored fallback corpses lack those recipes; enlarging model
admission does not supply them. Arbitrary addon rigs are outside this
stock-model acceptance. This qualification covers grounded standing deaths,
not every animation phase, water state, vehicle attachment or network mode.
