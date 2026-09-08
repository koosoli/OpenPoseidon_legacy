# Desert impact shadows (2026-09-08)

## Cause and fix

Ordinary rifle strikes create CraterDustSmall cloudlets. SampleShadow previously
classified every visible legacy cloudlet as dense smoke, submitting even this
brief dust to the smoke ground-shadow field (default strength 2.50). This produced
broad dark patches around impacts, particularly obvious on pale Desert sand.

ea3c0494 adds a per-source eligibility flag copied into each emitted cloudlet.
Crater dust is eligible only for smoky explosions, not ordinary impacts, blood or
water craters. Existing crater flags reconstruct the policy on save loading,
without changing the save or network format. Other sources retain their default.
The same predicate controls volumetric substitution, so excluded particles retain
their original billboard presentation instead of becoming dense volumetric smoke.
Crater decals, impact physics, particle motion, wind and downwash are unchanged.
This is not a global smoke-shadow disable and adds no GPU pass or texture sample.

## Verification

- PoseidonGame and PoseidonTests build successfully.
- `[Cloudlet]`: 31 assertions / 3 cases. Source defaults and policy wiring are
  checked; these are not full world/particle simulation tests. An initial test
  tried constructing a Cloudlet without game type data and crashed, so it was
  replaced with source-state and code-contract checks.
- `[Smoke],[Cloudlet],[effects]`: 443 assertions / 17 cases passed.
- Installed baseline 169059df versus ea3c0494, both process exit codes zero.
- `.tmp-bulb/impact-ground-before.png/.log` and
  `.tmp-bulb/impact-ground-after.png/.log`: repeated BulletSingleW terrain impacts
  on worlds/intro.wrp at x=9500, north=3500, terrain approximately 29.835 m.
  The broad shadow to the left of the impact disappears; the dust sprite and tiny
  crater remain. After: 36-66 live cloudlets but zero submitted smoke-shadow blobs,
  with legacy shadows still enabled and strength still 2.50.
- Capture uses a private profile, fixed exposure 0.1 and auto-exposure off on both
  sides to avoid clipping the pale sand. This is not a default-exposure acceptance
  test. Neither user graphics settings nor foreign Cargo.lock was changed.

## Fixture pitfalls

The initial x=2000/north=2000 site was shallow water, not dry Desert. A terrain
scan found dry land. Camera script camSetPos/camSetTarget use terrain-relative
heights; adding the sampled height again aims over the impact. Spawned projectiles
are explicitly positioned with setPosASL above the sampled ground. Start impacts
early and allow 20 seconds for the capture; earlier screenshots preceded impacts.
Discard the intermediate impact-*-before captures other than impact-ground-before
as acceptance evidence. Startup timing/particle populations are not deterministic,
so no pixel-perfect or performance claim is made.

Remaining general smoke containment/performance work stays PARTIAL in the ledger.
