# Upstream follow-up verification, 2026-10-04

Root follow-up branch: `codex/modernisation-followup-20261004`.
Immutable excavation backup: `codex/backup-excavation-20261004` at `8b93dec7`.
Public source audited: `ofpisnotdead/main` at `b36cddc7` and the 54 fetched open PR heads.
The owner's supplied audit remains in `docs/roadmap/modernisation/upstream/`.

## Combined native checkpoint

Engine source through `567dd086` built successfully with the Windows Clang
RelWithDebInfo preset: PoseidonGame, PoseidonTests, PoseidonCoreTests,
PoseidonServerTests and PoseidonFoundationTests. Matching game/renderer staging
completed; this paragraph does not claim installed runtime acceptance.

Author-run results:

- Windows crash-context parent regressions: 2 cases, 20 assertions, including
  context ending at an unreadable memory boundary. Existing diagnostic crash
  notification and Linux reporting were preserved.
- Path admission, LOD input, decoder, particle height, logging and signed rotation:
  33 cases, 4,895 assertions (`upstream-combined-native.log`).
- Harness restart, pending dispatch and nonreading client teardown: 4 cases,
  24 assertions (`upstream-harness-native.log`).
- Retail PAC default process: 3 cases passed, 156 assertions; the fourth case
  explicitly skipped because it requires the opt-in archive source process.
  Fresh `WGR_GEOMETRY_PAGE_RETAIL_SOURCE=1` process: all 3 PAC cases passed,
  184 assertions. The new marker-bearing PAC's synthetic AI88 channel values
  decode through the real VFS factory without ordinary PAA source admission.
- MLOD/P3DM non-regression: 51 cases, 1,432 assertions. This does not replace
  prior independent-reader evidence or certify outstanding winding/material
  visual claims.
- Trident native tests after canonical client resolution: 82 passed.
  `cargo build -p tri --locked` passed after repairing two stale dependency
  entries in Cargo.lock. The previous lock reproducibly refused `--locked`.

Logs are in the ignored root worktree `build/` directory. No Linux execution,
macOS execution or hosted Actions result is claimed. Linux copy-mode/long-path
fixtures need execution on Linux.

## Evidence maintenance

CORE-005's source hash became stale when the retained lint workflow's checkout
Action was updated. The ledger jobs and acceptance/refusal gates remain present;
all workflow YAML files parse. Recomputing its hash preserves the prior review
and status rather than claiming a new owner review.

AST-011B's source hash includes EngineWgpu.cpp. Excavation ceiling changes and
later renderer integration change this file without changing its P3DM parser
contract. Model loading regressions above and the separately recorded installed
excavation checks pass. Its hash will be refreshed with that limited scope;
UNVERIFIED source winding and PARTIAL runtime material coverage stay unchanged.

The full ledger validators are still blocked by older stale source evidence,
starting at CORE-NEG-002 (its ABI source has changed since the historical
verification). A full read-only sweep also found stale records outside this
upstream slice. They are not silently rehashed or promoted: each needs its own
contract review. This checkpoint therefore does **not** claim that the whole
modernisation ledger validates. CORE-005 and AST-011B alone were refreshed.

## Installed runtime

The matching executable/renderer was deployed at `e0c9df01` (03:11). Existing
local Steam Demo data supplies the imported `.Demo` fixtures; the executable
and renderer come from the stamped retail install. No game data was downloaded
or committed. At that checkpoint drop-height and fired-nearest-object passed,
with normal shutdown and unchanged hashes. Receipt:
`build/upstream-runtime/20261004-031328/result.json`.

The strict Turbo fixture stopped before readiness at WorldSetup.cpp's
ValidateOutVehicle assertion. Source review proved the same ordering defect in
the earlier port base: stable compaction retained moved actors in the normal
list tail until Resize, while validating their absence before Resize. `b979acc9`
defers the same guard until after compaction, preserving callback and reference
ownership. The installed `cdf0dba3` rerun passed all four fixtures, including
Turbo and ordered notebook hover audio. Each owned game reported normal
shutdown, no GPU/unhandled exception, and the executable/renderer hashes stayed
unchanged. Receipt: `upstream-followup-20261004-runtime.json`, copied from
`build/upstream-runtime/20261004-034045/result.json`. Existing Demo assets were
used with the matching retail executable/renderer; no assets were installed or
downloaded. These bounded fixtures do not certify every port's gameplay domain.

Additional decoder preservation: 49 cases passed, 5,784 assertions, two
explicit opt-in skips. Fresh `WGR_PAA_LZO_REPLAY_CACHE=1`: all five replay
cases passed, 105 assertions. Shared physics/corpse non-regression after the
automatic source integration: 72 cases passed, 5,045 assertions, including
actual articulated mass, contact-family, motion, probe and stale-handle cases.

Layered fog and automatic ragdolls have separate integration/runtime gates.
The Everon house report remains unreproduced; see the measured placement
diagnostic, which makes no house-placement fix claim.
