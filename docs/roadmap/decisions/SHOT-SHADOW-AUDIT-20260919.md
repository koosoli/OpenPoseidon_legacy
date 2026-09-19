# Independent shot-effect shadow audit — 2026-09-19

Implementation follow-up (owner subsequently authorized fixes): c6a95f19 with
89c3df3f excludes Man::MGunClouds from smoke shadows and keeps the separate
WeaponLightSource unchanged. Integrated equivalent: 962bebac + 6992a839 on
codex/combat-fixes. Its WGPU run Shadows-20260919-132406, in the combat-audit
worktree, observed 11/12 ordinary muzzle cloudlets with zero shadow blobs;
intentional SmokeShell still produced 108, then 439, then 512 blobs. The rifle
screenshot retained the visible effect and ordinary soldier shadow. Source and
the shared SampleShadow flag cover volumetric collection; no separate volumetric
runtime acceptance is claimed. The original investigation below is historical.

Investigation only; no production rendering changes. Branch render/shot-effects-shadow-audit-001 from verified parity HEAD **29f3b479c6256b742480d84b1cae43c695ffc929**. Separate from ai/combat-audit-001; neither changed the owner's installation. Attachment reference 4a2a30b4 predates four additional commits (listed in the AI report). Definitions below were inspected at the actual baseline.

## Source-confirmed path and verdict

Ordinary infantry gunfire reaches Soldier::FireWeaponEffects, SoldierOldAI.cpp:1278: AmmoShotBullet starts **_mGunClouds** for .1s and **_mGunFire** for .05s, plus the flash phase/frame effect. This method also services the shared effects path used for player/AI shots; emitter policy must not depend on control mode.

Man loads MGunClouds / MGunFire profiles in SoldierOld.cpp:709. SoldierOldSim.cpp:1118 updates the sources at the gun position. WeaponCloudsSource (Smokes.cpp:1477) → CloudletSource::Drop (:1140) creates a Cloudlet and propagates the source's **_smokeShadow**, whose default is **true** (Smokes.hpp). No ordinary muzzle-source opt-out is set here. WeaponFireSource also inherits WeaponCloudsSource; its brief emitted particles must be checked independently of its WeaponLightSource. Do not assume an emissive-looking particle is automatically excluded: SampleShadow's actual predicate has no explicit temperature/material-name gate.

Cloudlet::SampleShadow (Smokes.cpp:1085) rejects _smokeShadow=false, otherwise tests alpha-derived density (> .02), and returns position, radius and height. SmokeSystem::CollectShadowBlobs (SmokeSystem.cpp:286) samples the world's cloudlets alongside intentional SmokeVolume sources. The sampler's source flag is therefore the important semantic policy boundary.

EngineWgpu.cpp:12728 publishes up to 512 collected blobs into the **cloud/smoke ground-shadow** path. This is separate from normal object cascade shadow maps. :12749 collects up to 2048 blobs with force=true for optional **volumetric smoke injection**; force bypasses the ground-shadow toggle, but it does not bypass SampleShadow's emitter flag.

The normal cloudlet mesh already carries **NoShadow** (Smokes.cpp:836; particles also :2486). SceneShadowPass.cpp:171/:348 rejects NoShadow and object CastShadow opt-outs. Thus setting the normal mesh NoShadow again would not address the independently collected smoke blobs.

**Source-confirmed candidate cause:** ordinary muzzle cloudlets are eligible for smoke-ground-shadow blobs by default. A screenshot alone cannot establish which puff/flash/impact produced a dark area. Runtime controlled attribution is recorded below; no behavioral fix has been made or claimed.

## Minimal independent fix proposal

At the semantic ordinary infantry muzzle-effect setup, opt the rifle **_mGunClouds** source out using the existing SetSmokeShadow(false). Also exclude the purely visual **_mGunFire** cloudlet component if the controlled emission confirms it contributes (its base source has the same default); preserve its WeaponLightSource operation. Do not globally change WeaponCloudsSource defaults: explosion fire/dark-fire and other effects reuse that class.

Policy propagation already exists: source → Cloudlet → SampleShadow. That single predicate is used by both the ground shadow collector and forced volumetric collector. In pure volumetric mode, Cloudlet::Draw (:669) suppresses billboards **only when SampleShadow succeeds**. Consequently an opted-out muzzle effect remains visible as its original billboard instead of disappearing when excluded from the volume. Its existing lighting, alpha, receiving behavior and light emission need not be altered.

Keep smoke grenades, established substantial smoke, blast/explosion effects and all non-shot sources on their existing policy. Do not classify by texture, color, radius, duration or distance; resources can be shared. Preserve soldier/weapon/environment caster flags. No C++/Rust ABI change, save format, gameplay smoke visibility or projectile change is necessary for the proposed source classification. Existing per-impact opt-out at Smokes.cpp:1696/:1800 is a precedent, not permission to alter all explosions.

WGPU is the active path. Shared cloudlet behavior means the future setter must be checked against any fallback sprite drawing, but no new GL33 smoke shadow feature is proposed. Revert the small emitter-initialization change to roll back independently of any AI work.

## Reproduction and acceptance procedure

scripts/Run-ShotShadowAudit.ps1 -Scenario Shadows -Reference runs a read-only copied installed EXE/DLL with -C retail data, independent user profile, game lock, 1280×720, preset3, MSAA4, vsync off, cap60, fixed exposure .15, smoke mode0. Exact args/environment/hashes/stamp are in build/audit-runs/Shadows-*/provenance.json. The smoke fixture is generated only inside the isolated test world.

The harness fixes the camera, records an empty baseline, fires repeated M16 shots through the player's actual fire command, creates a SmokeShell positive-control effect, then combines the effects. Captures empty.png/rifle.png/smoke.png/mixed.png and the existing **SMK-036** collector counts provide attribution beyond dark pixels. Scripted player firing exercises effect emission; it is not an autonomous AI safety test. Combat branch runs cover autonomous AI emission through the same Soldier::FireWeaponEffects, but a matched camera AI burst capture is still a separate acceptance case.

For the future patch, repeat against unchanged reference with identical loaded config and lighting:
- rifle single shot and burst, player and AI: puff still visible; ordinary effect contributes zero smoke shadow/occlusion blobs;
- intentional smoke alone: collector contribution and visible shadow retained where reference shows one;
- mixed same-resource smoke: only ordinary shot contribution excluded;
- scene soldier/gun/environment shadows, receiving/lighting and muzzle light preserved;
- pure volumetric and both modes: opted-out puff remains billboard-visible; intentional smoke remains volumetric and shadowing;
- verify no conventional cascade effect is being confused with the smoke map and no frame/history mismatch explains the capture.

No after-fix capture is possible within this audit because production changes are expressly out of scope. If short single-shot collection falls between the 2s diagnostic reports, repeat a bounded burst or add passive emitter-tagged diagnostic counters; do not infer “no contribution” from a missing log line.

## Changed files and limitations

This workstream adds only its report, a harness script and the small isolated combat_audit.Intro fixture reused as an effect stage. No source/render behavior was changed or built on this branch. Runtime uses the unchanged installed reference copied into this worktree; the owner's normal installation stays untouched. Captures/logs are local build artifacts. Runtime results below distinguish observed from proposed conditions.

## Actual runtime result

Shadows-20260919-124519 completed exit0. Inspected rifle.png and smoke.png: ordinary effect darkening is visible near the shooter; the smoke control has a much larger ground shadow, and the soldier retains a normal shadow. Empty baseline collector0; rifle-only period at12:45:43.686 has8 cloudlets /5 smoke-shadow blobs, returns to0 as the effects expire. After SmokeShell creation, counts grow to147 then479, eventually hitting the512 collector cap. The mixed phase is therefore saturated and cannot quantify the additional rifle contribution; it is a coexistence capture, not proof of selective exclusion.

This **runtime-reproduces ordinary shot-effect eligibility** independent of the intentional smoke control. The source path narrows it to muzzle cloud/fire cloudlets; their individual contributions were not separately tagged. The test does not prove every dark pixel came from muzzle smoke alone, and no after-patch exclusion/lighting-preservation claim is made. Player shots were exercised through the fire command; AI emission was observed in the separate combat test, but matched AI shadow captures, a synchronized automatic burst and volumetric-mode visual controls remain untested. The procedure above specifies acceptance before a future fix can be called complete.

The existing renderer ledger's smoke completeness note was updated without changing status or adding a system. Its YAML parse check remains blocked by a **pre-existing baseline structure error**: HEAD already fails at line691 (RSYS-TERRAIN sequence under the wrong top-level structure); current file reports the same error four lines later because of this added note. Not repaired as part of the effect audit. PowerShell syntax and git diff whitespace checks pass. No game process left running.
