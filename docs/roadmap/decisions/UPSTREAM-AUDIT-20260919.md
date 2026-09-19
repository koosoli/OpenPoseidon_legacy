# Upstream review for the WGPU parity branch

Fetched `https://github.com/ofpisnotdead-com/CWR-CE.git` main on 2026-09-19:
`ce7c153746d659359260934c5470e46650e81f83`. The preceding audit used
`0e24d744`. Git contains seven new commits in that range; author dates alone
would miss the terrain work, authored earlier but now part of main.

Compared against `parity/foliage-lighting-001` at `4a2a30b4`.

| Commit | Decision for this fork |
| --- | --- |
| `77f1f8ce` addon warnings | Adopted the two LOG_WARN replacements in AddonSystem.cpp. Renderer-independent; missing dependencies/version requirements become visible in logs. No addon acceptance policy change. |
| `8c61786d` proxy land clipping | Do not cherry-pick. Calls GL33 LandClipInVS/SetLandClipParams, absent from this branch. The corresponding WGPU proxy/conform concern remains a separate rendering regression case; this commit does not solve it verbatim. |
| `82147189` instanced terrain | Skip port. Primarily PoseidonGL33 terrain/texture-array/heightmap architecture. WGPU already has its own terrain batching/CDLOD path. The upstream 10x CPU terrain claim is not transferable to our renderer or AI mission. |
| `b20613b0` GLSL validation | Skip: validation accompanying the GL33 terrain implementation. |
| `b534fd79` terrain shader count | Skip: updates the same GL33 shader accounting. |
| `c1f76798` terrain toggle location | Skip: moves the GL33 terrain toggle in the dev UI. |
| `ce7c1537` MP version label | Useful small UI follow-up, deferred: two decimal places show version differences more precisely. No performance benefit and no MP UI runtime check in this task. |

The previous audit's unfinished items are still unfinished, not silently adopted:

- Investigate invalid path coordinates (#237) and driverless static-weapon crew
  reassignment (#209) with focused regressions before changing AI behavior.
- Recheck mount/unmount lifetimes rather than importing GL33 texture teardown.
- Keep near-target timing, deterministic group ordering and numeric rounding;
  do not trade perception or determinism for speculative AI throughput.
- Nearbyint/SIMD and broad validator removal still lack workload evidence.

This task instead addresses the visible helicopter cadence directly and the
renderer-independent quadratic target-membership scan (AI-DISTANCE-20260919.md).
No upstream terrain merge or GL33 expansion was performed.

## Expanded coverage after owner follow-up

The seven-commit delta above was not a complete incoming-history audit.
[UPSTREAM-COVERAGE-20260919.md](UPSTREAM-COVERAGE-20260919.md) now inventories
all 84 incoming main commits: two patch-equivalent, 82 individually accounted
for, including relevant missing audio, Unicode/mod, mission-loading, logging,
download and UI changes. It distinguishes old unmerged PR-head candidates.
This follow-up changes documentation only; installed owner-test build remains
`f380053b`. Recommendations are not claims of already-integrated fixes.
