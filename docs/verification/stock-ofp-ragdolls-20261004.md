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

## Runtime acceptance

Pending integration and installed testing. Required witnesses for each model
are an ordinary death, an actual eleven-body/ten-joint Box3D articulation,
finite retained animation/point/proxy consumers, supported ground settlement,
and a real projectile that changes the retained corpse pose and resettles.
Subset runs are diagnostic and do not establish the complete denominator.

## Existing limits

The single-player local-ownership, water/attachment/removal, finite-motion and
physics allocation limits remain in force. Four simultaneous articulations
and 64 retained corpses are performance limits independent of model support.
Budget fallback uses authored death animation. Existing frozen runtime poses
retain their validated wake recipes; older authored/save corpses do not gain
one merely by enlarging model admission. Arbitrary addon rigs are outside this
stock-model acceptance.
