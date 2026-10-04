# PHY-040 — Bounded grounded prone contact recovery

## Observed problem

Installed `db119af0` on original Everon's reported hillside admitted the three
standing stock models but refused actual prone source poses. Western and Eastern
pelvis source vertices cleared the native terrain; their forearm proxies reached
36.07 and 44.43 mm initial penetration. The civilian source pelvis itself reached
54.74 mm penetration and its complete proxy census reached 90.38 mm. Removing
the generated pelvis AABB would not repair all three cases. The installed receipt
is `build/stock-ragdoll-models/reported-hill-prone-witness-20261004-183353`.

## One bounded candidate

Permit the real contact solver to recover at most 100 mm initial overlap only
for a pending, eligible automatic capture with current native land contact and
root clearance between -25 and +300 mm. Every body origin must be strictly above
both native and registered solver floors. Every existing vertex, floor-ray,
25 mm native/solver agreement, source-anchor, rig, extent and model identity gate
still applies. The explicit settled diagnostic keeps its original positive
clearance; unsupported or airborne captures retain the previous admission.

The candidate neither displaces the root/skin nor changes hull dimensions or
joint anchors. Initial consumers remain exact. The existing 15 m/s, 20 rad/s,
support and quiet retirement guards remain unchanged; time alone cannot freeze
an airborne corpse. Penetration deeper than 100 mm still refuses ownership.

## Native evidence and remaining acceptance

The registered `[corpse-prone-contact]` test creates a controlled eleven-body,
ten-joint prone graph on actual Box3D heightfield terrain, in spherical and
four-hinge variants. This is a solver qualification, not a retail model visual
proof. It exercises 36.07, 90.38 and 100 mm initial overlap, verifies exact initial
body origins/axes, actual ground rays, finite all-part motion, supported quiet
retirement, final hull clearance and complete handle cleanup. Boundary controls
refuse nongrounded, distant-root, nonfinite and excessive-penetration admission.

The six native cases pass 16,170 assertions. Maximum observed speed is 1.360 m/s,
spin 6.322 rad/s and travel 0.187 m; every final hull bottom is at least 4.4 mm
above the ground. `SoldierOld.cpp` syntax and diff checks pass. These observations
do not establish recovery of the retail poses or general hills/building contacts.

Root must run one matching installed Western/Eastern/civilian DOWN campaign on
the same hillside and inspect exact initial consumers, supported recovery,
maximum motion/travel, real projectile reaction and no airborne retirement. If
that candidate is unsafe, abandon it and keep the accepted standing fix with
prone contact listed as unsupported. No broader depth or retirement tuning is
part of this experiment.
