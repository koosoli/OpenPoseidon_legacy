# Bounded refinement of the original muzzle flash envelope

Status: source candidate, CPU checks passed; installed first-draw appearance pending.
Base: `9527c006` on `codex/streaming-residency`.

The installed first-draw capture in `build/muzzle-light/firstdraw-new-20261001-030852-f39869/On` proves that the earlier warmed self-emitting stock M16 flash is visible. It also shows a large pale, regular six-petal flower. The original first-draw control in `build/muzzle-light/firstdraw-original-20261001-031031-2145b2/On` shows a much dimmer flash. The existing night body and terrain illumination remains separate from the sprite.

This slice revises the envelope of the exact audited `data/zasleh_front.01..03.paa` and `data/zasleh_side.01..03.paa` sheets only. Front sheets retain a compact hot centre, with unequal, softly shortened authored lobes and cooler orange edges. The existing three animation phases each receive a fixed envelope; there is no per-frame random texture rewrite. Side sheets retain a softly tapered source-bounded plume. Original sheet dimensions and every source admission condition remain unchanged.

The previous decision `MUZZLE-FLASH-APPEARANCE-20261001.md` required byte-exact authored alpha. This follow-up deliberately supersedes that part: each new alpha is less than or equal to its authored value, and every authored clear texel remains exactly clear. No source coverage is added. RGB is still derived from actual authored energy; an all-black source cannot invent a flame. The texture diagnostic now accurately says `alpha=authored-bounded`. The off switch `WGR_MUZZLE_FLASH_APPEARANCE=0`, unsupported paths, unsupported dimensions, and invalid inputs preserve the input byte for byte.

No geometry, transform, material admission, shader, UI-time flash lifetime, Fired event ordering, ammunition, local-light pulse, or projectile behaviour changes. The helper still runs on decoded stock sheet upload, rather than every rendered frame. Source alpha constraints apply to those input texels; filtered or generated mip appearance still needs the normal installed rendering checks.

## Verification

- Standalone clang-cl/Catch2 run of `[muzzle-flash]`: **3 cases, 114,838 assertions passed**. Checks cover exact admission, byte-exact ablation, malformed input, bounded alpha, clear holes, authored-energy preservation, hot centre, compact asymmetric silhouette, and pairwise phase variation. The existing `std::getenv` UCRT deprecation warning remains in this pre-existing environment helper; no warning policy was changed.
- The compiled production helper processed all six original decoded retail sheets from the prior read-only muzzle audit. **73,728 actual texels** passed alpha-never-increases and clear-texel-exact-zero checks. The three actual front phase outputs are pairwise distinct.
- Front alpha sums become 236,545 / 190,469 / 238,826, from 1,074,893 / 895,764 / 1,074,893. Their nonzero footprints become 3,204 / 2,932 / 3,260 texels, from 7,675 / 6,782 / 7,675. This is a substantial intentional reduction of the broad flower and should be checked for sufficient visibility in the actual first draw.
- Side alpha sums become 366,495 / 318,908 / 243,573, from 538,271 / 447,185 / 271,303. Every sheet stays within the authored silhouette.
- `git diff --check` passed. No GPU test, game build, deployment, or installed run was performed by this agent.

The isolated worktree retains the actual production-helper converter, test commands, retail statistics, and comparison image under `build/muzzle-natural-cpu/`. In the comparison, columns are original decoded sheet, previous warm candidate, and this refinement. Original decoded source assets remain read-only in `C:/Users/mail/.codex/worktrees/muzzle-flash-visual/CWR-CE-paavohuhtalafork-/build/muzzle-audit/`.

## Required installed check

Run the existing ordinary-shot first-draw harness with the candidate and original switch control at the same front/body pose. Inspect the actual first-frame flash for a readable hot core, irregular short tapered spokes, and reduced flower outline. Retain actual ammunition/Fired evidence, returned screenshot path and hash, the later expired frame, and the night body/terrain illumination control. Inspect a side view and daylight shot as well. The offline sheet preview is useful design evidence; it does not establish the in-game perceived size, muzzle alignment, expiry, filtering, or appearance acceptance.
