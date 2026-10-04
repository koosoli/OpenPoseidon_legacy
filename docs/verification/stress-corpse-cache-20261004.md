# 63MAX corpse reaction and live terrain cache locks

Owner launch: installed `OpenPoseidon.exe --render=wgpu --dev --addon-root`
`Mods/@63units --test-mission Mods/@63units/63MAX.west_ls2`.
Installed source when investigated: **f8127fe5**. Candidate cache repair:
**2c396217**, branch `codex/stress-corpse-cache-20261004`, based on `793d7b96`.
The candidate is built/native-tested and safety-pushed, **not installed**.
Owner-handoff process 5104 remains running; preserve it under CLAUDE.md's
"Never Stop-Process a game you did not start" rule. Game-close request is pending.

## Primary corpse explanation

Read-only census of the exact local mission: **708 units**: 650 `SoldierEB`,
57 `OfficerE`, one `SoldierWB`. The name refers to groups, not 63 soldiers.
The local `west_ls2.pbo` contains only its WRP, map picture and world config;
its config does not override vehicle models.

The actual installed base CONFIG.BIN declares `MC vojakE2` for SoldierEB,
`mc vojakeo2.p3d` for OfficerE, and `MC vojakW2` for SoldierWB. All three
ragdoll model-admission checks in SoldierOld.cpp (1057, 1288, 1606 in this
source) accept only `data3d/mc vojakw2.p3d`. Therefore the 707 Eastern units
cannot get the automatic articulated prototype or its retained wake recipe.
This is the direct source explanation, not a claim to have replayed the
owner's individual shot. Default ON is admission ON for the supported
prototype; it does not mean every infantry model is supported. Previous
owner-facing wording overstated that coverage.

Separate limits: four active/pending rigs and 64 retained corpse poses.
Death admission falls back to authored animation if either budget is full.
A frozen supported runtime corpse refuses wake while all four active slots
are occupied. A budget-fallback/authored corpse has no runtime wake recipe.
Dead units loaded from older saves, water/contact/motion/model refusals are
additional distinct paths. Increasing limits alone cannot admit Eastern models.

## Eastern-model read-only audit

Both Eastern models decode with the existing local stock rig inspectors:
25 source anatomical/equipment bones, eight facial groups, 11 LODs, 37
single-bone normalized proxy bindings, all 15 required anatomical boundaries,
12 anatomical Fire Geometry components. They fit current point/LOD bounds.
Inspector controls: **15 tests PASS**. East soldier/officer collision arrays
match one another but differ from the Western model. No retail model, mesh,
texture or animation is committed by this audit.

These are plausible specific extensions, not runtime-qualified rigs. Retain
per-model exact palette/point/proxy capture and existing floor, contact,
hinge, motion and budget gates. Before promotion require actual Eastern and
officer capture, fall/ground settlement, real projectile wake and save/load
checks. No whitelist or solver bounds were broadened in this turn.

## Lock-cache defect and current fix

Independent of model support, runtime terrain impacts/editing call
Landscape::FlushCache. It formerly deleted the lock cache while AILocker
owners retained raw LockField pointers. Their later release targets the new
empty cache, causing the reported warning and risking stale pointer access.
The fix retains nonempty occupancy while other terrain/render/path caches
refresh; unloaded empty caches can still be recreated.

[Focused source/native evidence](lockcache-runtime-flush-20261004.md)
records an actual pre-fix failure and passing replacement object tests.
Root then built the combined committed Game/Tests tree: **17 steps PASS**.
The rebuilt actual PoseidonTests `[lock-cache],[heightmap-view],[ObjectID]`
suite passes **1,200 assertions / five cases** (`build/stress-lockcache-native.log`).

Installed acceptance is still pending: run the exact map/mission with matching
new EXE/DLL, make repeated real terrain crater/edit transactions while AI is
alive, allow movement/unlock, check warning/panic/access failures and normal
shutdown. Read actual runtime model identity and supported US control separately
from Eastern refusal. Do not claim the warning gone from the installed f8127fe5
build or an Eastern corpse reanimation fix. Pending closure is an external
runtime constraint, not a passing checkpoint.
