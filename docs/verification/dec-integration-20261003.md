# Dec branch integration — 2026-10-03

Integration branch: `codex/dec-integration-20261003`, based on the latest
`codex/streaming-residency` commit `31b73f95`.
Remote: the private `safety` mirror. The public origin is not updated.

## History and scope

All 98 fetched active `safety/dec/` tips are ancestors of the integration.
The exact names and full tip hashes are recorded in
[the ref inventory](dec-integration-20261003-refs.json). This includes all twelve
branches named by the owner, the older showcase branch and the Tidewater series.
Older Tidewater work was already in the streaming baseline.

* `19daf67c`: merge the current AI branch, which carries sinkhole texture
  imports, terrain holes, cave lighting, swimming and the source fixes.
* `e3c02737`: diagnostics merge, retaining the existing execution records.
* `0d766e72`: join the six standalone fix histories. Before this history-only
  merge, each branch had zero novel patches under `git cherry`: its source
  changes were already represented by patch-equivalent commits in the first merge.
* `43d6a0e5`: merge the old showcase history while retaining the newer W12e
  mission revision already shipped in the streaming baseline.
* `cd0ef8de`, `8d5d6ccc`: adapt existing renderer test fixtures to the combined
  shaders; production shading is unchanged by these fixture commits.
* `48fbf279`: installed smoke caught the retained shader reading the soft-ground
  storage buffer through cave lighting. Cave roofs now query authored terrain;
  an actual Naga entry-point resource test guards the eight-storage-slot limit.

## Conflict decisions

The later rain, puddle, mud/sand geometry, tree/roof snow, grass, muzzle lighting,
streaming and opt-in Box3D corpse work is retained. Incoming code augments those
paths rather than replacing them with older versions.

The incoming blend flag collided with the current snow receiver flag:
BLEND_SECTION is now 8192, SNOW_RECEIVER remains 2048, and GROUND_RECEIVER
remains 4096. The C++/Rust renderer ABI is 20 so the paired binary change is
explicit. Cockpit routing retains both the incoming inside-vehicle identity
check and the current explicit inside-LOD admission.

The cave resource binding is added beside the current signed snow/mud/sand
buffer. Object shading keeps existing weather/material paths while applying
incoming cave direct/ambient light gates. The post-optics diagnostic explicitly
refuses underground packets rather than claiming to reproduce the new cave
tonemap in its older outdoor probe.

The renderer fixtures now request the same five bind groups as production,
normalize Git CRLF before census assertions, extract the actual shared
ground-normal helper, and verify the current rough-soil reflection and raw
rain-coverage paths. Negative/support/geometry cases remain present.
The narrow helper/census fixes were supplied by the soil_snow_contacts and
puddle_optics subagents; the root agent applied and tested the combined changes.

## Asset and experimental boundaries

The swim animations are made by Claude (owner confirmation in writing, 2026-10-10) and ship
as the `@OP_Swim` default pack (`content/default-packs/swim/`). Without the pack, soldiers
keep the documented seabed-walking fallback.

The optional CfgAIFork group B (gradual spotting, hunting, grenade interval and
exposure) remains off by default, preserving the author’s unresolved results.
Cover spacing and splash safety are unconditional, as are the group A/C fixes.
A separate config-only test addon can opt into group B; it is not installed by
default. Existing source credits and the
Tidewater MIT notice remain intact. No specific conflicting source licence
was identified during this integration. The upstream Malprave patch set is
unavailable, so the earlier X-Ray source chain could not be independently
compared. This is not blanket public-distribution clearance. The detailed
[feature/test guide](dec-feature-test-guide-20261003.md) records the boundaries.

Cave-hole assets and a licensed swim addon were unavailable for an installed
feature-specific walkthrough. The imported records retain their original
unverified/known-failure states. The drawn-water grid readback grows to a
64×64 grid; its additional readback cost has not been benchmarked here.
The current Box3D heightfield is not promoted to cave-hole-aware collision.

## Verification

Installed behavior commit: **48fbf279**, stamped 2026-10-03_23:33.
The paired EXE/DLL hashes and bounded campaign summaries are recorded in
[the runtime receipt](dec-integration-20261003-runtime.json).

* Combined `PoseidonGame PoseidonTests` build: PASS; final game rebuild: PASS.
* Native texture/hole/diagnostics: 276 assertions in 22 cases, PASS.
* Native corpse/contact/hinge/freeze: 2794 assertions in 14 cases, PASS.
* Full serial renderer library suite: **687 passed, 0 failed, 5 ignored**.
* Explicit real-device post-optics test: 1 passed, including stale-bind refusal.
* Installed diagnostics: real mission, unit/group commands, marker/event JSON,
  pause clock, five fixed ticks (0.085 s), screenshot and normal exit 0, PASS.
* Installed opt-in Box3D articulation/hinge/freeze/lifecycle preservation: PASS.
* Optional AI addon mount/configuration consumption: PASS, actual C++ log
  confirms hunting=1 and grenadeInterval=5. This is not a firefight acceptance.
* Renderer ledger: PASS, 24 systems/82 production call sites.
* Compat-C1 ledger checker: still fails the existing AST-011B evidence-hash
  mismatch (12 evidence files); historical hashes were not rewritten to claim
  new acceptance. Imported tickets are not promoted to owner-validated.

The first installed run was rejected for a real invalid retained-object pipeline.
It was corrected in 48fbf279 and rerun with no logged validation/device/crash
errors. A preliminary optional-config probe used an unsupported SQF getNumber
operator; the runner now verifies the actual native configuration receipt,
and the final run exits normally. These failed attempts do not count as passes.

This is integration verification, not owner
appearance acceptance, broad AI mission acceptance or completion of the older
weather/terrain appearance backlog.
