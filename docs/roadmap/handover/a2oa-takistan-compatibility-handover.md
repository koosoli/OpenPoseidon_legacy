# Handover: Arma 2 / Operation Arrowhead compatibility slice

## Mission

Implement a bounded, evidence-backed **Arma 2: Operation Arrowhead Takistan**
world and asset compatibility slice without regressing the established OFP or
Arma 3 paths. This is not permission to claim general Arma 2 support: every
claim must name the tested format revision, asset family, and fixture.

The official, licensed reference corpus is local-only at:

`C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\inspiration\officialarma`

It includes official OA packages `takistan.pbo`, `takistan_data.pbo`, and
`takistan_data_layers.pbo`, as well as OA model/material packages. Its source
link is `inspiration/officialarma/sourcelink.txt`. The corpus is licensed input:
**never commit, copy, extract, redistribute, or hash raw asset content into the
repository.** The existing reference-corpus mechanism is local-only by design.

## Git and workspace safety — do this first

The compatibility path is deliberately not a separate renderer or a stale
worktree. Work in the current shared source branch: revision-specific readers
feed one common World, Model, Material and WGPU renderer path. Do not replace
the installed-game executable with a binary from another branch or worktree.

1. Preserve unrelated dirty work; do not reset, clean, delete, or reformat it.
2. Make focused commits only after the relevant build and regression checks.
3. Push commits only to the private safety mirror, never to `origin`,
   `poseidon-wgpu`, or `upstream`:

   ```powershell
   git push safety HEAD:refs/heads/<current-branch>
   ```

4. Do not create a pull request unless the owner requests one. Keep temporary
   extraction/inspection folders untracked; licensed reference assets must
   remain local.

## Architectural rules

Do not add Takistan-, `ca\\`-, filename-, path-, or map-specific conditionals to
the shared renderer. Instead:

- introduce an explicit world/material **source-generation or convention**
  descriptor selected by measured file version and authoring data;
- retain common material facts in a lossless IR (stages, TexGen/UV transforms,
  render state, texture references); profile-specific code maps that IR to
  terrain or object semantics;
- keep OFP's existing world placement behaviour untouched;
- keep A3 OPRW25/TerrainSNX behaviour isolated behind its own convention;
- make A2/OA transform, terrain, material, and texture-resolution choices a
  distinct convention with an evidence citation beside each choice;
- preserve unknown fields and fall back safely rather than guessing.

The current A3 code contains names such as `A3TerrainMaterial` and OPRW25-only
WGPU descriptors. Refactor only where a neutral abstraction genuinely removes
cross-generation coupling; do not rename unrelated working code merely for
style. The A3 path must remain byte-for-byte equivalent when its profile is
selected and its optional authored terrain experiment remains disabled.

## Required discovery phase

Before implementing a loader, produce a short decision record under
`docs/roadmap/decisions/` with measured facts:

1. Enumerate the local official PBOs and verify which archive contains the
   Takistan world file and its exact world/container revision.
2. Inspect the Takistan terrain material references and determine the actual
   terrain material convention, stage roles, TexGen use, texture extensions,
   and texture-resolution requirements.
3. Select a minimal, fully resolvable official OA fixture set: one terrain cell
   or small viewpoint, one static building, one vegetation object, and one
   material from each rendered family encountered.
4. Record the denominator: referenced models/materials/textures, resolved
   assets, parsed assets, and deliberately unsupported assets. Do not report
   a percentage without its numerator and denominator.

Use the repository's existing local corpus configuration:
`.reference-corpora.json` and `scripts/build_reference_fixture_index.py`.
The older corpus decision
`docs/roadmap/decisions/AST-007-reference-fixture-ladder-20260808.md` is
important context: base Arma 2 PBOs are not present, but the supplied OA/Takistan
packages are valid official evidence for this bounded OA slice.

If Takistan's world cannot be located or parsed, stop that implementation phase
with a reproducible evidence record rather than inventing a format layout. You
may still land parser/census tooling that is independently tested.

## Implementation scope

Deliver the smallest vertical slice that can render a real official OA Takistan
view through the production WGPU renderer:

1. Add version-dispatched A2/OA world parsing only after the discovery record
   identifies its exact format revision. Unknown revisions must reject safely.
2. Mount and resolve the supplied official OA PBO prefixes using the existing
   generic bank resolver; fix resolver defects generically, never by hard-coded
   archive names.
3. Translate A2/OA terrain data into a dedicated source convention. Do not force
   it through A3 TerrainSNX assumptions. Start with the proven safe terrain path;
   retain raw authored data for later multilayer work rather than flattening or
   fabricating it.
4. Preserve A2/OA object world transforms according to measured source
   semantics. Do not reuse either OFP auto-grounding or A3 authored-elevation
   behaviour without evidence.
5. Use the existing RVMAT IR and introduce only evidence-backed mappings for
   the selected A2/OA shader families. A normal/base-colour fallback is valid;
   unsupported material families must be visible in diagnostics and must not
   turn the whole frame black.
6. Add `--test-world` / dev-test support only if it remains generation-neutral
   and does not alter normal OFP startup.

## Tests and regression gates

Add synthetic unit tests for every new binary-version dispatch and every
generation/convention selection. Tests must not require local licensed assets.

For the local corpus, record optional evidence only. At minimum run:

```powershell
cmake --build build\win-x64-clang-rwdi --target PoseidonGame
.\build\win-x64-clang-rwdi\tests\unit\engine\Poseidon\PoseidonTests.exe "[Formats]" --reporter compact
.\scripts\run_preview0_wgpu_check.ps1 -BuildDir build\win-x64-clang-rwdi -SkipBuild
```

Then run a controlled installed-game WGPU smoke for the OA test world. Capture
day and night from deterministic coordinates, inspect the images, and compare
against the pre-change OFP baseline. Record:

- no crash / no full-frame black output;
- terrain, a static building, and vegetation are present at the intended height;
- no black terrain-material patches or texture binding overflow;
- OFP Preview-0 smoke passes unchanged;
- A3 OPRW25 parser and material/TexGen unit tests still pass.

An interactive owner smoke is required only for subjective visual acceptance
after automated checks establish the above properties. Leave the controlled test
session open if requested; do not take control of the owner's mouse.

## Documentation and handoff

Update the authoritative `docs/roadmap/status-ledger.yaml` with a new bounded
capability entry or a precise existing-ticket update. Include the source
generation, format revision, fixture denominator, automated tests, evidence
paths, known gaps, and the exact fallback. Add a decision record describing
the source convention and any transform/terrain-material choices.

In your final handoff, state:

- branch name and pushed commit SHA;
- the exact OA capability now proven;
- what remains unsupported and why;
- every build/test/smoke result;
- whether an owner visual judgement is still needed.

Do not claim “Arma 2 maps supported” unless a real official map reaches the
production renderer with the recorded acceptance gates. The expected first
claim is no broader than: **official OA Takistan, exact measured world revision,
and the specific tested terrain/model/material capabilities.**
