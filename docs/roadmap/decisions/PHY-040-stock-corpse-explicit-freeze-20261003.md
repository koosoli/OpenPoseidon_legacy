# PHY-040: explicit owned corpse freeze

Baseline `fcb9ad7b`: root's installed measured-hinge campaign `017a07` passed
11 bodies / 10 joints / 4 hinges, exact initial 33-bone palette / 542 compared
points / 37 proxies, bounded movement and clear / expiry / hide / delete cleanup.
This patch is a source/native candidate; its installed runtime acceptance must
be recorded by the sole game owner after the matching build and deployment.

`triPhysicsShowcase "corpse-freeze"` explicitly retires a currently valid owned
articulation. It releases joints before bodies, clears all handles and retains
Man's existing two affine phases, synthetic bone deltas, captured object transform
and validated culling bounds. It does not resample a newer solver frame or alter
the skin. Duplicate freeze and frozen impulses are refused. The frozen controller
performs no body readback. Shadow cache invalidation occurs once at transition
and once when that frozen owner is discarded.

The ordinary hold and active articulation retain their original 30-second lease
and rollback behavior. Only an explicitly frozen pose persists beyond that
deadline. World/backend generation, local dead-entity identity, model/skeleton,
animation references, moves/blend, monotonic clock, dry-ground restrictions,
exact object transform, hiding/deletion and position-change invalidation still
apply. No automatic death hook, automatic expiry freeze, save/network support or
default activation is introduced. Original spherical and measured-hinge controls
remain available. Freezing preserves existing authored skin burial; it does not
establish broad terrain, external convex contact or weapon/culling quality.

The existing read-only `corpse_pose_state` getter now returns every actual
post-correction palette point and supported AnimatePoint coordinate with its
source index, alongside complete palettes/proxies and retained bounds. Existing
stock model/LOD/4096-point/128-bone admission bounds remain in force. Unsupported
synthetic tails are explicitly enumerated as skipped; a missing consumer array
cannot be treated as equivalence.

## Bounded fixture generation invalidation

One test-only `corpse-frozen-world-retire` operation requires paused local single
player, valid frozen owner, the exact still-owned bounded 129x129 / 0.25m terrain
pointer/generation/origin/mutation serial, zero tracked bodies/models/probes and
zero articulated handles. Actual `b3World_GetCounters` solver totals must show
exactly one terrain body plus an optional positively identified separately owned
kinematic player proxy, with zero solver joints. Only that proxy may be removed;
the operation rechecks exactly one raw terrain body / zero joints before destroying
the fixture backend and reports its real changed generation. It does not clear
the frozen owner first. Foreign worlds, map corpora, terrain mutation attempts,
extra bodies and ambiguous ownership are refused. This proves fixture generation
invalidation, not actual map replacement. Tracked `stats.bodies` excludes both
terrain and this proxy; it is not used alone as an emptiness witness.

Raw upstream counter collection reads ID-pool sizes, cached tree heights, fixed
graph/manifold buckets and per-worker cached data; it performs no body/grid walk
at our accepted single-worker setting. Mutation serials conservatively advance
before a backend terrain set/patch attempt, including backend refusals.

## Checks and required installed campaign

Private actual backend/native checks pass: 2,819 assertions / 17 selected
corpse-pose, articulation, mass-update and replay cases, including real 11/10
ownership retirement, both nonrigid affine phases and synthetic tails, actual
weighted point/proxy equality after over 30 seconds, bounds retention, fresh-body
safety after generation change, raw proxy totals and terrain mutation attempts.
Measured hinge controls also pass: 2,302 assertions / 4 cases. No performance
cases or device tests were run by this source owner. SoldierOld.cpp and the
existing HarnessBuiltins.cpp getter pass clang-cl syntax checks. PowerShell AST
parsing and a historical actual census with explicitly mocked new arrays pass;
missing full consumed arrays are refused. Those grammar checks are not pixels or
installed pose measurements.

The sole game owner runs, from the merged worktree under the game lock:

```bash
LOCK_OWNER="explicit corpse freeze" scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-CorpsePoseProof.ps1 -ArticulationSmoke -AnatomicalHinges -FrozenRetirementSmoke -Label explicit-owned-corpse-freeze
```

The runner requires exact full palettes/points/proxies/root/bounds before and
after freeze, zero articulated handles, more than 31 simulation seconds of exact
retention, explicit clear, actual backend generation invalidation and getter
rejection, the unchanged active 30-second expiry control, then frozen hide/delete
cleanup. Its screenshots are available for the root's appearance judgment.
Installed acceptance and broader ragdoll behavior remain open until that receipt.
