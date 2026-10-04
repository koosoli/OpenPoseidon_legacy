# 63MAX corpse reaction and live terrain cache locks

Owner launch: installed `OpenPoseidon.exe --render=wgpu --dev --addon-root`
`Mods/@63units --test-mission Mods/@63units/63MAX.west_ls2`.
Installed source when investigated: **f8127fe5**. Candidate cache repair:
**2c396217**, branch `codex/stress-corpse-cache-20261004`, based on `793d7b96`.
The candidate is built/native-tested and safety-pushed, now **installed in
bdba184a at 12:57**. The owner explicitly authorised closing the old demo;
process 5104 was closed normally before deployment. bdba184a also repairs the
optional swim pack's animation transition after scripted dry-land relocation.

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

Installed acceptance at bdba184a: the exact original addon root and mission
loaded **708 units / 63 groups**. Four actual terrain paint transactions near
the Eastern infantry advanced the native height revision 6 -> 88 -> 170 ->
251 -> 332. Restoration advanced it to 452 and restored the sampled vertex
exactly. An actual SoldierEB was commanded to move after the edits and moved
**35.077 m**. All 708 units remained alive. The engine exited normally with
code 0 and no invalid-unlock warning, access violation or GPU failure.

Actual nearby shapes include `data3d/mc vojake2.p3d` and
`data3d/mc vojakeo2.p3d`, confirming the source exclusion applies to this
runtime. This check does not qualify either Eastern ragdoll, replay the owner's
corpse shot, or establish every cache owner lifetime under arbitrary missions.
The separately passing native regression establishes the retained pointer/
occupancy invariant.

The first stress probe used unsupported binary `nearestObject` syntax and
the engine's test mode exited with a script error. Corrected the probe to the
existing unary command and reran only the stress arm. That test defect is not
reported as a cache/runtime pass. The prior swim arm completed successfully
and exited normally; the final evidence combines those two independently
completed arms with the same installed binary hashes.

Reproduce with `scripts/Test-SwimAndLockCache.ps1 -ExpectedCommit bdba184a`,
under `scripts/with-game-lock.sh`; `-Checks stress` runs only this arm.
The local swim addon is needed only for the swim arm. Evidence:
`docs/verification/swim-cache-installed-20261004-runtime.json`.
