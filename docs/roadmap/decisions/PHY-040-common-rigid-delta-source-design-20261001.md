# Common rigid corpse delta: source design pending exact readiness

This is a read-only design audit, not a moving pose implementation. The actual
unqueried paused image sequence still fails exact readiness. No pose setter,
physics body, joint, root transform or default death change is authorized by
this document. The next proposed proof remains one explicitly selected local
single-player, dry, settled stock western corpse, under the existing bounded
lease. Contacts and articulated rig admission remain false.

## Why an owned final geometry snapshot is needed

`Man::Animate` prepares a corrected affine palette, applies the real synthetic
face bones and `AnimationRT::ApplyMatrices`, then calls `BasicAnimation`.
Graphical, special and one-point paths have distinct arithmetic. The installed
stock measurement found 33 matrices and 6,470 points across eleven levels.
With packed float xyz storage, positions and normals alone require about
152 KiB; separate legacy point results add about 76 KiB. Eleven full
33-entry palettes add about 17 KiB on a 64-byte matrix layout, plus
small level/proxy/identity records. A hard one-corpse budget of a few hundred
KiB is credible; use existing bounded level/point admission and explicit total
byte/count limits using actual `sizeof` and capacity rather than multiplying
those per-LOD maxima without a cap. Aligned vector storage increases these
estimates and must be accounted for explicitly.

Multiplying the 33 palette entries by a rigid delta is insufficient. GPU
unweighted vertices use a separate reserved world-only bone in
`EngineWgpu::SetSkinData`; CPU unweighted vertices bypass palette transforms.
Also the held unweighted `AnimatePoint` path returns shared `Shape::Pos`.
The one-point `Animate` writes that answer back to `Pos`, while one-point
`Deanimate` returns immediately. Reapplying a delta to that mutable value would
accumulate motion. Owned captured authored points avoid this feedback.

Preserve each actual consumer's authored value, including any affine residual
and its arithmetic order. Do not replace it with quaternionized phase bones
or make the graphical and special point APIs artificially identical. The
observed small graphical point/palette discrepancy is evidence of those
distinct paths; its identity limit must not change.

## Proposed private state and capture transaction

The existing phase lease remains the identity, lifetime and fallback authority.
An additional private complete-pose record would own per-level final authored
positions, normals and effective final palettes; actual supported point-path
answers; level-role/count identities; and the existing correction state. It
would retain the actual model, skeleton, weights and animation references,
never a global shared mutable palette. The rigid delta starts at exact identity.

Capture must explicitly force the existing CPU-posed route only within a
scoped capture operation. `RequestFaceCpuPose` currently controls the route;
otherwise `ApplyMatrices` can skip CPU work after a previous GPU-skinned draw
and a read of `Shape::Pos` need not be this corpse's current geometry. Run the
actual production `Animate` sequence for every admitted LOD and copy its final
positions/normals after `BasicAnimation`. Restore temporary routing state and
deanimate under the same owner. Validate finite geometry, finite palettes,
point counts, role coverage, conservative bounds and lease identity before
publishing the whole owned record. A failed capture publishes nothing.

Record actual point-path answers separately, outside a partially transformed
shared shape. Compare unweighted supported points before/after geometry capture
and refuse inconsistent source state. The existing two synthetic phase-tail
points are not safe calls to legacy `AnimationRT::Point`: phase matrices have
25 entries whereas the final graphical palette has 33. Their real graphical
consumer is covered by the actual final geometry snapshot. A future caller
asking for those indices needs an explicitly defined final-palette point path,
not a fake parent copy or an unchecked legacy phase lookup. Do not claim that
undefined legacy point API behavior has been preserved.

## Consumer application and identity arithmetic

At exact identity the ordinary existing rendering and point/matrix paths must
run without added matrix multiplication, CPU routing or normalization. This
is necessary for exact authored pixel equivalence, not merely approximate
geometric equivalence.

For a finite, admitted nonidentity delta, graphical and special `Animate`
would restore the complete owned final positions/normals and apply the delta
once to positions and its orientation once to normals. Force the existing
CPU-posed backend for that selected owner so unweighted vertices and face
positions reach both color and depth. Do not invoke `BasicAnimation` again
after restoring a snapshot that already includes its work. Invalidate the
vertex/normal and convex-component buffers as the existing CPU deformation
path requires. Special/memory/one-point queries return the rigid transform of
their owned authored answer, never the previous mutated shared shape.

`AnimateMatrix(weight)` also serves as an internal head-orientation
intermediate. `BasicAnimation`, `BasicDeanimation` and the selection overload
must use a private authored-matrix helper preserving the existing blend and
orthogonalization. Public complete weight/selection results apply the delta
only after their existing correction sequence. The inherited
`EntityAI::AnimateProxyMatrix` calls the selection overload, so it receives
the delta there exactly once; `GetProxy` must not add another delta.
`SoldierOldAI::HeadAimBasis` is an external weight-matrix consumer to audit,
even though the selected corpse normally has no active cargo seat.

The complete diagnostic would expose effective delta-times-authored palettes
and actual post-delta geometry/point/proxy values. Independent limb deltas
would require later residual/mapping work; this common delta does not pretend
to solve their mixed-weight discrepancy.

## Bounds, shadows and retirement

The Man-specific enlarged authored box and sphere are currently returned by
`AnimatedMinMax`/`AnimatedBSphere`, independent of actual posed vertices.
Transform all eight box corners conservatively and transform the sphere
center while retaining its radius for a proven rigid delta. Refuse excessive
or nonrigid deltas; do not rely on spare authored bounds to hide the motion.
The root entity transform, networking, save fields and simulation ownership
remain unchanged. This is a small model-space proof, not corpse relocation.

Projected shadows cache settled corpses through `ShadowPoseFrozen` and
`IsAnimatedShadow`. While a nonidentity pose is owned they must be dynamic.
Currently `CorpsePoseRefusal` itself calls `ShadowPoseFrozen`; naively making
that return false would retire the lease immediately. Split the actual
authored-death-settled predicate from the cosmetic shadow-cache policy, keeping
ordinary behavior identical. Confirm dynamic projected shadows do not replace
the stored authored static entry; clear/expiry must return to the authored
shadow, not a cached displaced silhouette. The WGPU shadow collector already
calls `Animate`/`Deanimate` on nonstatic objects and therefore must consume the
same complete CPU snapshot. Actual installed color/depth/shadow checks remain
mandatory.

Clear all owned final-pose state on explicit clear, lease expiry, hiding,
deletion, movement, model/skeleton/weight/animation replacement, world change
and serialization. Restore any temporary shared-shape deformation before
retirement when it is actively borrowed; nested consumer use needs a bounded
scope, not an inverse transform guessed after the owner disappears. Invalid
or stale data returns entirely to authored behavior. No stale partial palette
or last-mutated unweighted point survives.

## Review and implementation gates

Proposed ownership stays in `CorpsePose.hpp`, the corpse regions of
`SoldierOld.cpp/.hpp` and `SoldierOldSimProxy.cpp`, existing production pose
tests and narrow harness/proof regions. No renderer/shader, physics solver or
cloth/contact region is proposed. The internal/external head-matrix split may
require a tiny `SoldierOldAI.cpp` call-site audit; request that ownership if an
edit becomes necessary. No source changes implementing this design exist yet.

Before implementation, root must accept the complete transaction design and
resolve the exact read-only capture readiness failure. Then tests should
exercise actual production helpers with nonrigid authored data, mixed and
zero weights, one-point repetition, complete synthetic face output, nested
point/proxy queries, normals, transformed bounds, identity bypass and stale
lease/model/world/clear paths. They must test repeated consumers to catch
double application, not just a single mathematically similar matrix helper.
Installed opt-in screenshots, actual queried points/proxies, color/depth
shadows and every lifetime exit are required before accepting this proof.
Measured anchor gaps and existing hull penetrations remain observations;
none become calibrated spherical joints by inference.
