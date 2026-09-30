# Streamer parked at Oliver's request — 30 September 2026

## Checkout and safety

Continue only when Oliver resumes this scope. HISTORICAL branch at pause:
`codex/streaming-residency`, worktree `.claude/worktrees/combat-audit`. These are
not instructions to switch branches or discard subsequent work.
Code checkpoint `f0f09a0e` is committed
and pushed to `safety` (PoseidonWGPU-private-safety-mirror).
Authoritative plan: `../modernisation/OpenPoseidon_Streaming_and_Virtualized_Geometry_Plan.md`.
Whole plan remains OPEN; defaultReady=false/performanceAccepted=false.
Experimental geometry pages and DayZ owner material staging remain OFF by default.
Tidewater Native remains default; ocean prototype/native material work is parked.

## Mandatory restart discovery — preserve all intervening work

Oliver will work on other changes while the streamer is parked. The successor
must discover the current integration baseline, not blindly resume this old HEAD.
There may be several parallel branches; newest commit time is not proof of the
correct baseline. Treat every SHA, process, test result and path below as historical.

1. Read applicable AGENTS.md, `.agents/README.md`, smoke instructions and the
   current authoritative ledger. Inspect root and managed worktrees read-only:
   `git status --short`, `git branch --show-current`, `git worktree list --porcelain`,
   `git branch -vv`, `git remote -v`, and recent decorated history. Inspect the
   installed `DEPLOYED-FROM.txt` separately; it describes binaries, not necessarily
   the latest source branch. Inventory uncommitted/untracked work before changes.
2. Fetch the configured relevant remotes without switching/resetting any checkout.
   Compare local/upstream heads and ancestry. Identify Oliver's current integration
   line and its intervening changes; do not assume main, this parked branch, the
   installed branch or the branch with the newest timestamp is automatically right.
   Report the selected branch/SHA and the evidence before starting implementation.
   Ask Oliver only if the intended line remains ambiguous after this inspection.
3. Check whether the selected line contains `f0f09a0e` (last streamer code) and
   the parked documentation, using ancestry and diffs; equivalent cherry-picked
   changes may have different SHAs. Read newer ledger entries and code before
   redoing any task. A passing historical test does not validate intervening changes.
4. Reuse a suitable isolated worktree or create a `codex/` integration branch from
   the discovered current baseline. Preserve both newer work and missing streamer
   changes through a reviewed merge/integration if needed. Do not switch to the old
   branch as a shortcut, overwrite newer implementations, reset/rebase another
   worktree, clean untracked files, stash someone else's work, or force-push.
   Uncommitted user work stays untouched; if it is required for the baseline,
   establish how it should be included before a dependent integration/deployment.
   Resolve conflicts semantically and verify both features rather than choosing
   an entire old file. Keep recoverable refs for pre-integration committed heads.
5. Continue only the still-open streamer tasks on that combined current baseline.
   Commit/push before paired deployment. Follow the ownership/game-lock rules and
   verify the current installation belongs to or is included in the combined line.
   Never deploy the historical f0/b8 binary over a newer unrelated build. Re-run
   only relevant checks; preserve Tidewater and other intervening improvements.

## Copyable successor prompt

> Resume the OpenPoseidon streamer / virtualized-geometry plan. First read
> `docs/roadmap/decisions/STREAMING-PARKED-20260930.md` and the current authoritative
> plan and repository instructions. This handover is historical: independently
> discover Oliver's current integration branch and all intervening changes from
> worktrees, working-tree status, fetched remotes, ancestry and diffs. Preserve
> newer commits and user changes; do not blindly switch to codex/streaming-residency
> or reset to a parked SHA. Select or safely integrate a current baseline containing
> both newer work and the streamer, then state its branch/SHA. Verify the pending
> runtime gates and continue the remaining plan with bounded tests and performance
> checks. Subagents may work on disjoint files; only the root owns build/deployment
> and game/GPU tests. Reforger native anomalies are not material ground truth.

## Installed versus built

Installed EXE/DLL stamp observed at pause:
`codex/streaming-residency b8f61b97 2026-09-30_08:08 deploy-ps1`.
An owner-started OpenPoseidon PID27856 was running at08:44:40. Root did not launch
or stop it, overwrite binaries, or run another game. Therefore f0 deployment
and its runtime verification are PENDING. Resume by reading current stamp and
acquiring game lock; never assume this historical PID/stamp is still current.
Exact f0 postcommit PoseidonGame build PASSED13 scheduled steps. Evidence:
`build/stream-residency/f0-postcommit-game-build.txt`. Installed pair remains b8.
Pair deploy with scripts/Deploy.ps1 and Git Bash `C:/Program Files/Git/bin/bash.exe`.

## Established runtime evidence

b8 exact installed `world-residency-b8-wide-20260930-082218-9548d1` passed normal0,
no force. Actual parser/source/PAC-bound SkalaNew Fine250 -> certified Root125
at1.907304px -> actual Fine retirement -> returned conventional original0/page
absence BEFORE worker refill -> fresh Fine250 birth2/model4 -> Restore/Abort,
worker/debt0. One complete cycle only, original other-view model retained.
V3 Hausdorff upper3.206243m; no pixel, all-view, GPU completion or memory-gain claim.
Two narrower camera fixture runs correctly refused Root (>3px), normal0.
Private test WRP now64cells*50m/3200m, identical two installed-bank placements,
far camera3195 3195 30 225 -0.4. No installed source overrides.

Native Reforger4d fixed village: clean1/1,20683registered/4930main,
GPU30.4015ms versus previous31.121 ordered sample. DayZ4d OFF route clean1269
frames median23.044/p95117.5896/p99322.0221ms. Neither is causal performance
acceptance. Reforger native anomalies are not material correctness ground truth.

## New f0 code: built/unit tested, not runtime accepted

- Actual-world-only diagnostic GPU reference removed; original model lease stays
  separately owned. `retailDiagnosticReferenceAllocated` exposes this distinction.
- Three bounded residency cycles. Fine slot2 is reused only after exact complete
  retirement ACK, quiescent owner and zero debt; absent history compacted,
  new producer/model/page birth monotonic. Root remains. Upfront future queue
  reservation and cumulative caps remain. Rust model rows append: maximum five
  private model births, NOT unlimited constant-memory paging.
- Skala2 zero normals are accepted ONLY for vertices unused by all validated
  faces. Actual archive/source bytes remain exact; used zero/invalid index refuse.
- Exact DayZ tenement partial parent material stage: at most one parent section
  translation OR one unconditional current-source physical texture/update,
  128sections/128roles/64uploads/64MiB extra estimate/1.5s. Unsupported/stale/cap
  falls back to ordinary complete final registration before publication.
  Conditional/generated roles and proxies remain uncovered. NOT complete
  source closure, proxy preparation or reuse-only final registration.

Joint Game/Core/PoseidonTests build308steps passed. Candidate checks4983assertions/
8cases include external installed-bank SkalaNew+Skala2. Transaction/stable647/14,
final-Shape244/4 passed. Initial wrong target/tag attempts ran no cases and do
not count. Root script syntax checks passed.

## Resume sequence

1. Complete mandatory restart discovery above; then confirm stamp/game lock.
   Build the selected combined committed source (not the historical f0 binary) and
   deploy matched EXE+DLL+assets only after owner game exits.
2. `scripts/Test-RetailColdWorldSource.ps1 -Residency -Label world-threecycle-f0`
   now requires3cycles, compacted live-history counts, exact retirement ACK,
   fresh identities and durable original0 Restore before each refill. Any
   failure must be diagnosed; do not relax3px/4px or source/material proof.
3. `... -Records -Asset Skala2 -Label world-skala2-f0`; then actual-world cycle
   for secondasset only if current candidate/material/source proof passes.
4. DayZ useful staging screen:
   `Test-DayZStreamingLatency.ps1 -Mode Disabled -FramePaceTrace
    -DayZOwnerMaterialStage -Label material-f0`.
   Material switch implies PrimaryStage,ArchiveSourceBindings,WarmTextureJobs,
   DayZTextureTrace. Own current-upload/handoff/final remaining-upload witness
   is distinct from optional warm queue proof. Ordinary final may still stall.
   Compare against PrimaryStage+ArchiveSourceBindings+WarmTextureJobs+TextureTrace
   with material switch OFF; identical instrumentation/binary and counterbalanced
   runs if promising. An unexercised optional warm queue is not successful proof.
5. One native Reforger fixed-pose regression screen after behavior verified.
   No parallel game/GPU runs or CPU build during timing runs.

## Next design, not implemented

A bounded versioned derived GHP cache could avoid repeated CLOD bake, retaining
all current source/final-Shape/SVertex/Fine parity/material/epoch checks and V3
certificate rebuilt anew. Geometry agent held writes throughout final build;
NO cache helper/hook/tests were applied. Do not assume cache implementation exists.
Complete parent+proxy paced admission requires moving or deferring ShapeBank
installation: proxies are created inside NewFromModel before normal synchronous
WorldShapeLoaded/RegisterGpuModel. Do not pretend parent declarations prove final
bindings. Unlimited residency also needs safe renderer model-row lifecycle;
C++ history compaction alone cannot solve append-only renderer memory.

Subagents never ran build/game/deploy. At pause new cache work was explicitly
cancelled before writes. All code edits are in f0; pre-existing untracked
`.tmp-movecam/missions/` is not ours and must not be deleted or committed.
