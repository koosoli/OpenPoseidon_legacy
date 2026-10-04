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

Implementation replaces the Western-only checks with the
shared exact stock model and live rig capability checks. Twenty-two class-backed
models retain the four measured elbow/knee hinges. Fifteen sparse models use
the existing constrained spherical joints; Angelina's missing skin boundaries
use finite, clustered pairs from her own authored FireGeometry. This is
recorded separately from shared graphical-vertex evidence.

Final engine source `41ce4191` adds passive angular damping only to articulated
bodies, preserves native equipment proxy blends when all influences share one
verified physics part, and measures the spherical torso direction across the
complete body-centre separation. Captured skin, proxy matrices and joint pivots
remain unchanged. Initial exact consumer checks, ground support and all motion
safety checks remain in force.

The normal native target passed 16,136 assertions in 83 cases with filter
`[physics],[articulation],[corpse-pose]`. The supported paired deployment stamped
`41ce4191` at 16:37 local time. Ragdolls are default-on; the existing shared
Physics dev-panel toggle remains available.

## Runtime acceptance

One complete installed census run on `41ce4191` verified ordinary deaths,
actual eleven-body/ten-joint Box3D articulations, finite retained consumers,
supported initial settlement and actual native stock-bullet reactivation for
all 37 models. Thirty-five completed the additional post-hit settlement,
31-simulation-second persistence, frozen save/load pose and cleanup checks.
The final shared Physics toggle checks passed, the installed pair stayed
unchanged, and the game exited normally with code zero.

Full runtime acceptance is **not** claimed: `BISCamelPilot` and
`BISCamelPilot2` react to bullets but sometimes keep their left arm moving
afterward. A bounded repeat measured actual upper-arm/forearm angular motion
around 2-5 rad/s and unsafe freeze motion after about 60 simulation seconds.
It is not a missed hit or an unsupported model. The safety checks correctly
refuse retirement; time-only freezing or relaxed motion checks would conceal
the problem. Their joint/contact settlement remains open.

The owner requested a finite wrap-up. This batch stops with all stock model
admission implemented and those two post-hit settling failures recorded.
There is no further physics calibration or new feature expansion in this batch.

The compact source-bound receipt is
`stock-ofp-ragdolls-20261004-runtime.json`. Full evidence remains locally under
`build/stock-ragdoll-models/final-stock-all37-20261004-163923` and
`build/stock-ragdoll-models/stock-pilot-motion-20261004-170627`.
Earlier incomplete campaigns and observer failures remain preserved rather
than counted as successful acceptance.

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
