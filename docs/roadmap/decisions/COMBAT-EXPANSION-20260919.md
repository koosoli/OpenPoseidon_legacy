# Firing angles, partial cover, dense-scene costs and upstream integration

Work continues on parity/foliage-lighting-001 through the isolated codex/combat-fixes worktree. Baseline source b233b675, installed reference a30452b2. This report supplements AI-PROGRESS-20260919 and the full UPSTREAM-COVERAGE-20260919 inventory; it does not replace their historical results.

## Gameplay

Blocked autonomous SP infantry search deterministic alternating lateral positions at 8/16/24/32 m and bounded forward-diagonal approaches, subject to terrain/path availability, current target knowledge, normal movement permissions, eye/muzzle visibility and the existing full-flight friendly-fire envelope. If no immediate safe position is found, a candidate may reduce the number of allied bodies intersecting that envelope compared with the current position; this grants movement only. Four candidate queries per simulation timestamp and staggered retry intervals bound the work. Ordinary move/attack planning performs the movement; candidate approval never authorizes a shot. Stop orders, disabled movement, forced fire, player control and multiplayer retain their existing contracts. Existing attack orders can reuse the selected position for a bounded period. Improved remains the default, with Legacy in the AI tab.

Partial cover uses one alternate upper-body point only after the normal torso ray fails. Both the eye and muzzle must see it. The same target/weapon-specific point offset feeds aiming and aim eligibility; the normal knowledge, accuracy, leading, ballistics and emission safety checks remain. Visibility-cache entries now distinguish weapon and reserve, and expired entries are refreshed rather than duplicated. Perception may observe the same physically exposed upper-body point; it does not reveal hidden targets through cover. No shared Target aiming position is mutated.

## Performance

Visibility cells maintain a non-owning index excluding TypeTempVehicle footprints, which CheckVisibility already unconditionally rejected. Add/delete/clear maintain order and lifetime; all other obstacle predicates remain unchanged. The test includes 20,000 temporary objects mixed with eligible objects.

Ordinary text console output uses a bounded, draining worker queue. Original categories/timestamps/order survive; file/error/strict-mode and JSONL protocol output stay synchronous. POSEIDON_SYNC_CONSOLE=1 restores direct console writes. A full queue retains backpressure rather than dropping diagnostics.

First matched 63MAX far/near/far pair, 20–45 seconds process trace window (before final cover-cache refinement):

| Metric | Installed reference | Changed build |
|---|---:|---:|
| Frames in window | 508 | 663 |
| Mean frame delta | 49.089 ms | 37.610 ms |
| Frame p95 / p99 | 149.537 / 174.135 ms | 137.545 / 155.528 ms |
| Maximum frame delta | 250.211 ms | 189.656 ms |
| Mean simulation / draw | 41.347 / 6.364 ms | 29.574 / 6.249 ms |

Artifacts: StressDistance-20260919-163729 and StressDistance-20260919-163823, build/stress-distance-{before,after}.*.csv. All trace rows retained in those windows. Camera transitions occur at mission 11.543/19.839/27.132 versus 11.917/20.247/28.237, so this is a short paired observation, not a deterministic benchmark or a vanilla comparison. Long simulation frames remain. Vanilla-level smoothness is NOT established.

The previous helicopter stall included a 306 ms gap between consecutive console log lines. Suppressing only console output on the old installed binary yielded mean 17.091 ms, p99 17.507 ms, max 18.834 ms after startup (Helicopter-20260919-160130). That motivated the console queue; one clean run does not prove all helicopter jumps eliminated.

## Relevant upstream packages

All 21 TAKE-P1/P2 commits from the incoming-main inventory are integrated, based on upstream ce7c153746d659359260934c5470e46650e81f83:

- 230dd173, df5d30f5, 33fd6ce1: warning-only mission boot, UTF-8 Windows enumeration/archive extraction.
- de34f16e, d3f93039: abstract-type and shared-center radio logging.
- bab72482: OpenAL fractional fades, streaming offsets, stereo skips.
- d9192a25, ce7c1537, 0e24d744: HTML widths, multiplayer version precision, portable PAA tests.
- 9d362dd3, 16d39136: shared binding conflicts and bindable chat controls.
- fe737ea0, e88327cb, 6c35ff79, f6572308, 04189666: mission names/localization/codepages and world/addon validation.
- 4bcddd8f, b54e468e: multiplayer mission duplicate discovery and folder navigation.
- 90f38746, f2b39cd9, 613f5fde: mod feedback, resumable downloads, progress/ETA/install-failure feedback.

Adaptations: preserve the native --test-world override; skip oversized UTF-8 entries instead of returning truncated aliases and preserve Windows file metadata; do not warn for legitimate shared-center radio traffic; retain fork UI fallback translations and existing Unicode mission tests. The upstream download integration runner depended on absent infrastructure, so its three runner fixtures were replaced with a self-contained loopback HTTP regression in Run-CombatAudit.ps1. No GL33 terrain/UBO feature was imported. The separate WGPU review and older PR-head queues remain explicitly distinct from these TAKE packages; renderer-specific performance claims are not transferred.

## Verification so far

- PoseidonGame/PoseidonTests builds succeeded; follow-up committed-tree build required before deployment.
- 115 engine cases / 20,892 assertions: combat, interpolation, ballistics, logging, download, HTML and audio filters.
- 51 UI/archive/input-category cases / 779 assertions; 27 visibility-index/input-profile/chat cases / 307 assertions; 2 Windows enumeration cases / 13 assertions.
- The visibility-index fixture initially crashed because it lacked an initialized terrain. Corrected to allocate an actual flat test heightfield; production code was not changed to accommodate the test.
- Cover-20260919-163445: clear / partial / fully hidden / partial / clear gives **17 / 16 / 0 / 17 / 16** fired rounds. Diagnostic rays prove torso blocked and head exposed in partial phases, both blocked in hidden phase. Earlier low fences that did not obstruct the ray were rejected as fixtures. These stock-object results do not certify every terrain, posture or muzzle-blocked layout.
- Downloads-20260919-163622: successful byte-exact payloads after interruption, ignored Range, changed validator, and five consecutive interruptions. Request log proves resumed offsets and restart where appropriate. Loopback only, clean process exit.
- First reposition prototype moved about 7 m and fired 39 rounds after a blocked start; final surrounding-enemies regression remains to be recorded below.

No universal friendly-fire guarantee, complete vanilla parity, multiplayer gameplay claim or independent visual approval is made. Newly added upstream UI integration fixtures are retained; the complete UI/MP integration matrix has not been run in this bounded campaign.

Dense-scene iteration evidence: StressBehavior-20260919-164450 had blocked soldiers with no group-assigned attack target, bypassing the original automatic-attack hook. The hook now also uses the actual firing target. StressBehavior-20260919-164944 then exercised hundreds of probes but found no immediately safe destination: the all-or-nothing movement criterion itself deadlocked a dense ring. The planner now allows strictly fewer obstructing allies as an intermediate movement goal, with additional forward-diagonal samples. The emission gate still requires zero blockers. Candidate muzzle height uses the actual weapon point; infantry GetCombatHeight() is a zero-valued legacy range hint.

Final-cache cover regression Cover-20260919-165100 again gives 17/16/0/17/16. Crossfire-20260919-165201 gives 0/0/0/21 at the existing near/behind-target/far-behind/clear controls. Reposition-20260919-164354 moved 7.51 m and fired 45 rounds (before the subsequent dense-ring refinements). The final 64-case combat/logging/index/ballistics/interpolation filter passes 20,704 assertions.

StressBehavior-20260919-165344 exercises progressive movement: 1,250 candidate probes, 208 selections across 185 soldiers by the last snapshot; 130 of those soldiers have paths and 52 are moving. 707 infantry remain; two sampled emitted BulletBurstE tracks have negative dirY (-0.06757 and -0.03861). A sampled projectile list is not a complete shot counter. This establishes that crowded-ring movement no longer requires a completely safe destination before anybody may move, not that every possible enclosure has a safe shot.

Longer settling control StressBehavior-20260919-165537 (90 seconds wall observation): {"units": 707, "probes": 4344, "selections": 846, "selected_units": 390, "moving": 346, "sampled_shots": 128, "max_dirY": 0.11795739829540253}. All 707 infantry remain in the final snapshot; emitted shot samples demonstrate that fire resumes as the formation moves. Clean harness exit. These are sampled diagnostic counts, not a complete damage or shot-volume audit.
