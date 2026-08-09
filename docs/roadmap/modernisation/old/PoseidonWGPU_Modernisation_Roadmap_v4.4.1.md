# PoseidonWGPU Modernisation Master Roadmap

**Version:** 4.4.1
**Date:** 2026-08-01
**Audience:** AI implementation agents and human reviewers
**Status:** Frozen core roadmap with a machine-readable execution overlay, early compatibility proof, narrow water interoperability slice, and evidence-gated capability domains; future structural changes require implementation evidence.

## Execution status snapshot — 2026-08-02

This is a deliberately conservative handover checklist. The status ledger remains
the mutable authority; a checked item below means the implementation/evidence is
tracked in the repository, not that Preview 0 has passed its independent release
gate.

- [x] Canonical roadmap, execution overlay, checksum manifest, evidence, and status ledger are tracked under `docs/roadmap/`.
- [x] The Preview-0 ledger validator checks canonical paths, checksums, clean-manifest evidence, ticket schema, dependencies, and tracked evidence files.
- [x] Tier-1 WGPU build/startup, explicit-backend proof, ABI refusal, runtime diagnostics, capture/metrics, and initial visual-difference classification are integrated with repository evidence.
- [x] CI archives the executable, `wgpu_renderer` library, manifest, and available evidence for the matching build.
- [x] Local WGPU water timing and Zeus debug-tool stability work are archived as implementation commits and retained pending runtime smoke evidence.
- [x] The hosted Trident screenshot failure is localised and addressed in `dacdf77`. Each `triScreenshot` emitted one GL error, failing the I-20 gate on `flows/demo/credits` (2) and `flows/demo/mission_load` (3) — counts matching each test's screenshot count exactly, which places the fault in the capture readback. **The stated cause (a multisampled default framebuffer under Xvfb) is unverified and probably wrong**: `EngineGL33` never requests multisampling on its GL context. The pre-fix code also never bound framebuffer 0 before `glReadPixels`, so it read whatever post-FX left bound; `dacdf77` fixes that too. GL33 remains fallback-only — this restores capture, it is not parity work.
- [x] The Trident step bound is sized to the suite (35m step / 45m job). The 15m bound added in `816ec28` SIGTERMed run `91499679288` at exactly 15m with scenarios still queued, so it was cutting healthy runs short rather than catching hangs.
- [x] Zeus editing positions resolve through one helper against the engine's own in-game cursor (`bcbd74c`), and the pending Zeus executable is built and installed.
- [x] Zeus runtime side relations, cross-side combat, and death handling are covered by automated scenarios (`tests/integration/ai/`, `537e71c`) instead of manual play.
- [x] **Hosted CI confirmed the capture fix.** Run `30760957610` is the first Trident run ever to finish: 81 tests, **78 passed / 3 failed**, 1554.6s under `-j4`, inside the bound. `flows/demo/credits` and `flows/demo/mission_load` — the two I-20 GL-error failures that blocked Preview 0 — now pass. Only hosted CI could confirm this, and it has.
- [x] **All three remaining CI failures diagnosed against a local reproduction**, using the Demo package already installed at `D:\SteamLibrary\...\Arma Cold War Assault Demo` (no asset download was needed). Both campaign tests pass locally on Demo data serially *and* at `-j4` on 16 cores; oversubscribing to `-j12` reproduces the CI failure classes exactly — `HARNESS_PORT` spawn timeouts, UI assertions firing before the display settles, GL errors under many concurrent contexts. It is contention, not corruption: `triGetModsActiveSet` reads in-process UI state, not a shared file. Parallelism also scales poorly (serial >2100s vs `-j4` 1554s ≈ 1.35× for 4× concurrency, because the cost is game startup against a shared Xvfb display and disk), so CI dropped to `-j2`.
- [x] **A genuine GL33 readback bug fixed** (`70c4cf3`), separate from the capture path. `SampleBackBufferNonBlack` and `SamplePixel` called `ResolveSSAAToDefault()` then `glReadPixels` without binding the read framebuffer, so they read whatever FBO was bound; against a multisampled post-FX FBO every read is rejected and **the caller silently receives zero**. That is `ui/editor/editor_altenter_keeps_rendering`'s `NonBlackCount == 0` failure, which also fails hosted. `dacdf77` fixed the screenshot readback but not these two.
- [x] **Trident given a per-test `connect_timeout`** so startup has its own budget. Measured cause: WGPU init builds 38 pipelines from 43 WGSL sources with no pipeline cache — ~3s more than GL33 on a discrete GPU (7.4s vs 4.5s) and shader-compile bound, so it scales worst on the hosted software rasteriser where `shadow_map_scene_dump` times out at 90s. Raising `timeout` would also have lengthened command waits, buying a long tail on genuine failures.
- [x] **Trident `.seq` phases now inherit their sequence sidecar** (`e69583a`). Phases run one file at a time and have no toml of their own, so the whole `foo.seq.toml` — mission, binary, renderer pin, timeouts — was silently dropped and each phase booted a default game. The failure looked like a broken test rather than lost configuration.
- [x] **Water made observable to tests** (`WaterFrameStats`, `triWaterStats`, `triWaterNodeCount`) plus five fixtures: liveness on retail and Demo islands, a CDLOD bound, aerial LOD, and a two-boot lifecycle sequence. Water previously had no test coverage at all.
- [x] **`RND-030` renderer-plan reconciliation complete**, plus the water-plan and July-audit reconciliations. Five plans marked `PLANNED` describe systems that ship; two `WTR-` namespaces collide on six identifiers; the Water Master Plan carries no status markers at all.
- [x] **A stale-binary build bug found and fixed** (`36a9d29`). `cmake --build --target PoseidonGame` — the deploy command in `CLAUDE.md` — never ran the wgpu cdylib staging step, because it lived in an `ALL` custom target that nothing depended on. `dist/` kept whatever the last bare `cmake --build` left: a day old for `PoseidonGame`, twelve days for `PoseidonGameDemo`. Every build reported success. A stale cdylib does not crash (the FFI exports still resolve and appended `WgrWaterParams` lanes are ignored), so it presents as "the old settings work, the new ones do nothing" — which is what a smoke test reported, none of it renderer code. **Verify a deployment by content, not timestamp or hash-against-source: both were green while the file was a day old.**
- [x] **Water debug views 37 and 38 corrected** (`71051f4`), closing the `WTR-001` item above. The interaction field is `(height, velocity, foam)`, so `.x` is height: view 37 "Surface speed" was drawing `sqrt(height² + velocity²)` and view 38 was drawing velocity again on a second scale under a name for data nothing stores.
- [x] **`WTR-001` audit items answered with evidence** — eight of nine. See that ticket for the `WaterSurfaceState` dead fields, the slope-variance double-count, the cascade weights, SSR confidence, and a debug view that reports the wrong quantity.
- [x] **Zeus interactions verified by hand (2026-08-04)** — owner-confirmed working: cursor ownership in the focused viewport, lasso, copy/paste, group drag, and **mouse-wheel elevation** (Page Up/Down was moved to the wheel after it was found to collide with the free-fly camera's own `MoveUp`/`MoveDown` bindings). These are interactive mouse judgements and cannot be automated. Current build is installed at `D:\SteamLibrary\...\ARMA Cold War Assault\ColdWarAssault.exe`.
- [x] **`WTR-001` closed (2026-08-04)** — reflection ownership resolved. Whether cloud duplication is gone is a visual judgement no code inspection settles; the owner confirmed it directly rather than via debug views 40/42. Recorded as **owner-verified, not independently reviewed** — Preview 0 has no second human, so that distinction is what the reader needs, not a bare tick.
- [x] **Verification commits and evidence hashes are populated** (`8456fd5`): all six blockers carry `verification_commit: 85b6772`, the first workflow run to go green, and hashes derived by `scripts/compute_evidence_hash.py` rather than asserted — two of the values carried in the handover notes had already drifted. **Reviewers remain**, and are the only thing left on this line. Then move each Preview-0 blocker from `INTEGRATED` to `VALIDATED` only when its state-dependent gate passes. Note the review model: Preview 0 has **no independent reviewer and cannot obtain one** — Oliver Kay is the only human on the project, so review is owner-performed smoke testing recorded per ticket in `reviewer_method`. The authoring AI is deliberately never recorded as a reviewer, since the author cannot also be the independent check. `VALIDATED` here means the owner verified it, not that a second party did. **Done 2026-08-03**:
  each blocker records `reviewer: Oliver Kay` and a `reviewer_method` stating what was actually
  exercised, and all six are `VALIDATED`. Two of those methods deliberately decline the smoke-test
  claim — `CORE-005` is the ledger itself, which a running game cannot exercise, and `TEST-002`'s
  capture paths were not driven by hand — so the record says what each review did and did not cover
  rather than implying a uniform check.
- [x] Perform the reviewed **activation** pass before authorising `REL-000`. The **clean-checkout
  reproducibility** half is done (2026-08-05) and found four defects, all fixed, all of which
  failed silently: the build/install/run scripts were documented only in the generated `.qoder`
  wiki and not in `CLAUDE.md` (`d9ce33b`), so an install missing 64 files looked normal;
  `Build.ps1` could not configure from a clean shell because it never set up the Windows SDK, and
  presented as "the C++ compiler is broken" (`ef2cdfc`); a fresh clone failed checkout on Windows
  with `Filename too long` (`3a66939`); and `assets/` was never staged, so grass silently ran on
  its procedural fallback (`20eda5a`). Verified end to end: fresh clone -> `Build.ps1` -> full
  build -> `dist/` complete.

  **Activation performed 2026-08-05** (`3723892`): the overlay is `template: false`,
  `activation_state: ACTIVE`, `implementation_forbidden_until_resolved: false`, with a
  `bootstrap_policy.resolution` block recording each required check. It is late rather than early —
  all six blockers were already `VALIDATED` before the pass ran — so the resolution note states
  that the flags record work which happened rather than authorising work that had not.
  `REL-000` is promoted into `authorised_tickets` and `execution_queue` and has an `OPEN` ledger
  record. Its packaging commit is **not** automatically `HEAD`: see the caveat on that record.

## Version 4.4.1 execution-contract correction

- Preserves the Version 4.4 architecture, release order, compatibility proof, narrow water slice, semantic-physics rules, public framing, and evidence-driven agent freedom.
- Aligns the canonical Execution Overlay contract with Preview 0 Overlay schema Version 2: `preview_blockers`, `authorised_tickets`, `execution_queue`, `next_candidates`, and `named_holds`.
- Separates release blockers from currently claimed work and makes the live status ledger the only authority for mutable ownership, dependencies, and scheduling state.
- Defines queue state as bootstrap input only through `initial_scheduling_state` and `initial_blocked_by`; activation must never overwrite an existing live ledger record.
- Defines a minimal bootstrap lock ledger that may exist before `CORE-005`, then requires `CORE-005` to promote it into the complete authoritative ledger and CI contract.
- Keeps milestone roles canonical in the roadmap and ledger rather than duplicating potentially conflicting role values in the overlay.
- Makes large/small and cross-system WIP accounting machine-checkable and classifies cross-language ABI work as large, integration-sensitive work.
- Clarifies that the recorded execution baseline is the pre-activation code baseline, avoiding a self-reference with the commit that activates the overlay.
- Corrects the initial Preview 0 example so tickets become `ACTIVE` only after a valid claim rather than merely because they are authorised.

## 1. How agents should use this document

### Project identity and public framing — REQUIRED

The project should be described publicly as:

> A modern open-source evolution of the Poseidon engine for large-scale combined-arms sandbox games, preserving OFP/CWA compatibility and progressively supporting later Real Virtuality content and behaviours.

This statement is directional rather than a promise that every later-generation asset, mission, script, vehicle, simulation class, or network behaviour already works.

- “Reforger-quality experience” may be used internally as an aspirational benchmark for large-world combined-arms gameplay, rendering, streaming, physical interaction, Game Master workflows, networking, and mod tooling.
- Do not describe PoseidonWGPU as an “open-source Arma Reforger engine” in a way that implies Enfusion compatibility, feature parity, an official relationship, or drop-in compatibility with modern Arma content.
- Public status claims must come from validated ledger records and released capability matrices, not from roadmap intent.
- Agents may propose clearer wording, but it must preserve the independent-project disclaimer, honest compatibility scope, and distinction between quality ambition and compatibility guarantees.

This roadmap uses five independent governance fields.

### Obligation

```text
REQUIRED
SUGGESTED
OPTIONAL
```

### Execution mode

```text
IMPLEMENT
INVESTIGATE
VALIDATE
```

### Scheduling state

```text
OPEN
HOLD
BLOCKED
ACTIVE
DONE
```

### Lifecycle status

```text
PLANNED
RESEARCHED
PROTOTYPED
INTEGRATED
VALIDATED
SHIPPABLE
DEFERRED
SUPERSEDED
```

### Milestone role

`milestone_role` describes whether a ticket can delay the milestone or preview in which it is listed. It is independent of obligation, execution mode, scheduling state, and lifecycle maturity.

```text
BLOCKING
NON_BLOCKING_VALIDATION
CONDITIONAL_DEPENDENCY
OPTIONAL_PARALLEL
```

- **BLOCKING** — The named milestone or preview cannot ship until the ticket's approved acceptance criteria pass or the project owner approves an explicit fallback.
- **NON_BLOCKING_VALIDATION** — The ticket should be investigated or validated during the phase, but an unfinished result does not delay the release unless the ledger later promotes it.
- **CONDITIONAL_DEPENDENCY** — The ticket becomes blocking only when an activated implementation or approved decision actually depends on its result.
- **OPTIONAL_PARALLEL** — The work may run beside the milestone but is not part of its exit criteria.

A milestone checklist is therefore not, by itself, a list of release blockers. The ledger role and explicit exit criteria determine release impact. CI must reject milestone-linked ledger entries that omit `milestone_role`.

Examples:

```yaml
obligation: REQUIRED
execution_mode: INVESTIGATE
scheduling_state: OPEN
status: RESEARCHED
milestone_role: NON_BLOCKING_VALIDATION
```

A required investigation may validly end with `outcome: REJECTED`. A suggested implementation may be placed on `HOLD`. These fields must not be collapsed into one ambiguous “requirement level.”

Prose headings may contain convenient summaries such as “required outcome” or “suggested,” but the ledger fields are authoritative.

### Outcome versus mechanism — REQUIRED governance

The roadmap distinguishes **what the engine must achieve** from **how an agent currently proposes to achieve it**.

- Project-wide requirements, compatibility guarantees, authority boundaries, safety fallbacks, and accepted release outcomes are firm unless explicitly changed by the project owner.
- Named algorithms, data structures, middleware, APIs, pass layouts, storage formats, and architectural patterns are candidate mechanisms unless the ledger explicitly records them as fixed constraints.
- A ticket may require an outcome while leaving the implementation investigative. For example, stable temporal behaviour may be required without requiring TAA, one particular upscaler, or one global history manager.
- Agents must inspect the current production path before treating a roadmap suggestion as necessary. Existing code may already solve the problem, may make the suggestion obsolete, or may expose a better direction.
- A more capable agent may adopt, adapt, replace, split, merge, reject, or defer a suggested mechanism when it documents why the alternative is superior and preserves required behaviour, compatibility, multiplayer correctness, reversibility, and performance budgets.
- Agents must not downgrade a required outcome merely because a suggested implementation proves unsuitable. They should search for a better mechanism or record the issue as blocked with evidence.
- Agents must not implement a named technique simply because it appears in this document. Evidence and production value take precedence over architectural fashion.
- When two designs are plausibly competitive, prefer a bounded investigation and decision record over prematurely committing the engine to either one.
- Named third-party middleware is a candidate mechanism, not a project requirement, unless an approved decision record explicitly fixes it for a release scope.
- Middleware maturity, upstream features, licensing, determinism, and maintenance risk are time-sensitive. Agents must verify the pinned version and current upstream state rather than relying on this roadmap's historical description.
- Prefer one authoritative gameplay implementation for a capability. Running multiple physics, navigation, animation, or world-state backends in parallel requires exceptional evidence, a clear ownership boundary, conformance tests, and a plan for resolving disagreements.

### Constraint hierarchy

When instructions appear to conflict, use this order:

1. Explicit project-owner decisions and project-wide requirements.
2. Accepted gameplay, multiplayer, compatibility, data-preservation, and release-slice outcomes.
3. Verified production behaviour and measured platform constraints.
4. Ticket-specific acceptance criteria and approved decision records.
5. Suggested architectures and candidate techniques.
6. Examples, reference products, illustrative algorithms, and discovery links.

A lower level must not silently override a higher level. A demonstrably better lower-level mechanism may replace another mechanism while preserving the higher-level outcome.

### Capability-ticket release rule

New cross-cutting capability tickets added after Version 4.1.1 do not automatically block an existing preview or milestone. Their release impact is controlled by `milestone_role`, explicit dependencies, and the milestone exit criteria—not by appearing in a prose checklist.

- A capability ticket starts as `NON_BLOCKING_VALIDATION`, `CONDITIONAL_DEPENDENCY`, or `OPTIONAL_PARALLEL` unless the project owner explicitly approves `BLOCKING`.
- Promotion to `BLOCKING` requires a decision record explaining why the release outcome cannot be delivered safely through the current path, a specialised alternative, or an approved fallback.
- An investigation may validly conclude that the current system is sufficient, that a specialised solution is preferable, or that the work should be deferred.
- A stronger agent may propose a different role when evidence shows that the current classification is either too strict or too permissive. The integration owner records the final decision.

### Machine-readable Execution Overlay — REQUIRED operational layer

The master roadmap defines long-term constraints, accepted outcomes, capability domains, canonical milestone roles, and decision gates. It is **not** the daily task list. A small machine-readable Execution Overlay defines what agents are currently authorised to implement.

The overlay changes more frequently than this roadmap and should normally be updated without creating a new roadmap version. Mutable execution truth remains in the status ledger; the overlay supplies authorisation, initial queue input, WIP policy, and activation conditions.

Minimum schema:

```yaml
schema_version: 2
overlay_id: PREVIEW0-EXEC-001
revision: "1.2"
template: true

roadmap:
  version: "4.4.1"
  path: docs/roadmap/modernisation/PoseidonWGPU_Modernisation_Roadmap_v4.4.1.md
  sha256: <roadmap-file-hash>
  repository_commit: null

ledger:
  path: docs/roadmap/status-ledger.yaml
  create_if_missing: true
  bootstrap_schema:
    required_ticket_fields:
      - id
      - owner
      - branch
      - baseline_commit
      - scheduling_state
      - blocked_by
  authoritative_for:
    - owner
    - branch
    - baseline_commit
    - verification_commit
    - dependencies
    - blocked_by
    - scheduling_state
    - lifecycle_status
    - milestone_role
    - evidence

execution:
  current_preview: PREVIEW_0
  integration_target: null
  baseline_commit: null
  baseline_semantics: PRE_ACTIVATION_CODE_BASELINE
  integration_owner: null
  authorization_policy: DENY_UNLESS_LISTED
  activation_state: PREPARED

wip_limits:
  active_large_tickets_max: 2
  active_small_tickets_max: 3
  active_cross_system_tickets_max: 2
  count_only_state: ACTIVE

preview_blockers:
  - PERF-001
  - CORE-005
  - CORE-NEG-001
  - CORE-NEG-002
  - RND-005A
  - TEST-002

authorised_tickets:
  - PERF-001
  - CORE-005
  - CORE-NEG-001
  - CORE-NEG-002
  - RND-005A
  - TEST-002

execution_queue:
  - id: PERF-001
    initial_scheduling_state: OPEN
    size: SMALL
    cross_system: false
    priority: 1

  - id: CORE-NEG-001
    initial_scheduling_state: BLOCKED
    initial_blocked_by: [PERF-001]
    size: LARGE
    cross_system: true
    priority: 2

next_candidates:
  - id: REL-000
  - id: RND-030

named_holds:
  - id: PHY-GATE-0
  - id: WTR-100
  - id: WTR-200
  - id: TRN-010
  - id: FAR-000
  - id: CLD-010
```

Operational rules:

- A ticket not listed under `authorised_tickets` is not authorised for implementation, although an agent may submit a short proposal for later activation.
- `preview_blockers` describes what must reach an accepted lifecycle state before the preview exits; it does not make every blocker simultaneously `ACTIVE`.
- `execution_queue` provides initial queue metadata and bounded active scope. `initial_scheduling_state` and `initial_blocked_by` seed a missing ledger entry only; they must never overwrite live ledger state.
- The ledger is the sole mutable authority for ownership, branch, dependencies, blockers, scheduling state, lifecycle state, milestone role, and evidence after bootstrap.
- Milestone roles are canonical in this roadmap and the validated ledger. The overlay should not duplicate them unless a future schema defines an explicit assertion field that CI checks for exact equality.
- Before `CORE-005` is complete, bootstrap may create only the minimum lock ledger needed to claim work safely. `CORE-005` must promote that seed into the full authoritative schema and validation contract.
- A ticket becomes `ACTIVE` only through a valid atomic ledger claim. Merely appearing in `authorised_tickets` or `execution_queue` does not activate it.
- The overlay may further restrict work, but it may not override project-wide requirements, approved decisions, canonical milestone roles, ticket dependencies, or safety and compatibility constraints.
- All ticket IDs in the overlay must resolve to canonical roadmap tickets. Domain labels may appear only in descriptive comments, not as substitutes for ticket IDs.
- Every queue entry declares `size` and `cross_system` so CI can enforce large, small, and cross-system WIP limits from live `ACTIVE` ledger records.
- The integration owner may revise practical WIP limits when team capacity and integration evidence justify it, but the change must be recorded in the overlay.
- Promotion of an inactive capability ticket requires an owner, baseline commit, integration target, applicable canonical milestone role, and explicit reason for activation.
- Quick Wins must also appear in `authorised_tickets` and `execution_queue` before implementation.
- The committed execution overlay must name a real integration owner, integration target, and baseline commit before implementation branches begin; `null` values in the template mean “not yet activated.”
- `execution.baseline_commit` records the pre-activation code baseline. The later commit that writes the resolved overlay may therefore be its direct descendant.
- Preview 0 starts with exactly the six Build Truth entries shown above. `REL-000` becomes authorised only after the technical Preview 0 candidate is reproducible.
- `RND-030` is the first major post-Preview-0 candidate unless implementation evidence identifies a more urgent production blocker.

Some agents working on this project may have stronger domain expertise or reasoning than the authors of this roadmap. They are expected to form their own technical opinion. When rejecting a suggestion, document:

- What was verified in the current branch.
- Alternatives considered.
- Performance evidence.
- Compatibility implications.
- Multiplayer consequences.
- Migration and fallback strategy.
- Why the proposed replacement is better.

The roadmap is not permission to build speculative infrastructure forever. Every milestone must improve build trust, runtime reliability, measured performance, or a complete playable vertical slice.

---

# 2. Project-wide requirements

## 2.1 Multiplayer first — REQUIRED

Every gameplay-relevant ticket must define:

- Authority: server, predicted client, deterministic derived state, or cosmetic only.
- Fixed simulation tick and event ordering.
- Replicated inputs and outputs.
- Prediction, correction, and reconciliation.
- Stable IDs and idempotent events.
- Late-join reconstruction.
- Save/load representation and schema migration.
- Checksums or revision tracking.
- Bandwidth budget and failure fallback.

Different visual quality settings may change grass detail, foam, smoke curls, god-ray sampling, or reflections, but must not change hits, cover, collision, vehicle motion, smoke gameplay visibility, water submersion, AI navigation, or destruction.

## 2.2 Preserve original game data — REQUIRED

Original PBOs, worlds, models, missions, and textures remain read-only. Modernisation uses virtual overlays, derived caches, sparse delta layers, optional packages, and safe fallbacks.

## 2.3 Zeus remains in Debug Tools — REQUIRED

Zeus/Game Master must remain permanently available through the debug tools. Backend logic may be refactored into reusable services, but the debug version remains the most powerful developer and server-administration frontend.

## 2.4 No silent partial implementations — REQUIRED

A struct, shader, helper, compute pass, or debug slider is not a completed feature unless production code consumes it and it has tests, diagnostics, performance data, and a fallback.

## 2.5 Release value — REQUIRED

Every major phase must end in a working release slice. Agents should avoid months of foundation work that does not make WGPU more trustworthy, faster, more stable, or visibly more coherent.

---

# 3. Ticket classification and applicability

Each ticket declares one primary class in the status ledger:

```text
BUILD_INFRA          Builds, CI, packaging, ABI validation
RUNTIME_INFRA        Shared runtime architecture, renderer lifecycle, and canonical queries
NETWORK_INFRA        Networking primitives, reconstruction, and network test infrastructure
RESEARCH_DECISION    Bounded investigation or formal decision gate
AUTH_GAMEPLAY        Server-authoritative gameplay
DERIVED_GAMEPLAY     Predicted or deterministically reconstructed gameplay
PERSISTENT_WORLD     Saved and replicated world state
REPLICATED_COSMETIC  Replicated visual state without gameplay authority
LOCAL_COSMETIC       Local visual detail
OFFLINE_TOOL         Importer, cooker, validator, or asset/build tool
DEBUG_TOOL           Diagnostics or developer interface
```

Checklist entries use:

```text
[ ] Completed
[N/A — reason, reviewer]
```

Do not invent meaningless packet-loss, dedicated-server, or screenshot tests where they do not apply. Ticket class and applicability determine the required subset.

### Authoritative ticket metadata

The status ledger is authoritative for:

```text
class
obligation
execution_mode
scheduling_state
status
outcome
owner
branch
baseline_commit
verification_commit
milestone
milestone_role
dependencies
blocked_by
```

These fields are not required to be repeated under every prose heading. Prose describes scope and intent; the ledger determines current execution truth.

### Mandatory ticket completion checklist

The ID and title are already present in each canonical heading. All metadata fields are stored in the authoritative ticket ledger or linked implementation record and are not required to be repeated under every prose definition.

```text
[ ] Current production call path verified
[ ] Alternatives evaluated where applicable
[ ] Dependencies and blockers recorded
[ ] Multiplayer contract completed, or N/A with reason
[ ] Performance budget completed, or N/A with reason
[ ] Compatibility and fallback completed
[ ] Debug/diagnostic route completed
[ ] Applicable tests completed
[ ] CPU/GPU/memory/network measurements completed where applicable
[ ] Independent audit completed
```

### Ticket identity rule — REQUIRED

Each ticket ID is defined in exactly one place. Other sections and milestones reference that definition.

- [ ] CI rejects duplicate ticket IDs.
- [ ] Renamed tickets record the superseded ID.
- [ ] Gates and implementation tickets use different IDs.
- [ ] Ticket scope cannot silently change without a decision record.

### `INVESTIGATE` decision contract — REQUIRED

Before prototyping, every investigative ticket defines:

```text
Question being answered:
Current baseline:
Alternatives:
Success metrics:
Failure thresholds:
Maximum prototype scope:
Maximum time/resource allowance:
Evidence to collect:
Decision owner:
Permitted outcomes: ADOPTED / ADAPTED / REJECTED / DEFERRED / NOT_APPLICABLE
```

Research status and outcome remain separate:

```yaml
execution_mode: INVESTIGATE
status: VALIDATED
outcome: REJECTED
```

An investigation is complete when it produces an evidence-backed decision, not when it quietly becomes a production subsystem.

# 4. Milestone −1 — Build truth and release engineering

This milestone precedes feature foundations. Multiple autonomous agents cannot be trusted to improve the renderer if the build can silently select the wrong backend, combine mismatched binaries, or compile WGSL only during game startup.

## PERF-001 — Platform and performance targets — REQUIRED

Create a versioned platform table before feature budgets are accepted. For Preview 0, this may deliberately begin with **one realistic, continuously testable Tier 1 configuration**. A complete cross-platform hardware matrix is not required before execution begins; additional tiers should be added only when they can be meaningfully built, tested, or measured.

Each tier states:

- Operating system and support level.
- WGPU backend.
- Representative CPU.
- Representative GPU and VRAM.
- System RAM.
- Resolution and scaling mode.
- Target frame rate.
- CPU-frame budget.
- GPU-frame budget.
- Dedicated-server tick rate and tick budget.
- Expected player/entity scale.
- Network bandwidth target.
- Maximum persistent derived-cache size.
- Release-blocking versus advisory status.

Support levels:

```text
TIER_1_RELEASE_BLOCKING
TIER_2_SUPPORTED_NON_BLOCKING
TIER_3_EXPERIMENTAL
UNSUPPORTED
```

- [x] No required field remains `TBD` when the first feature budget is approved.
- [x] Metal or Linux failures block release only if that configuration is declared release-blocking.
- [x] Budget changes are versioned and justified.
- [x] Every major renderer feature declares its budget share against a named tier.

## CORE-NEG-001 — Reproducible WGPU build and startup — REQUIRED

- [x] `PERF-001` declares initial platform support before CI release gates are finalised.
- [x] CI builds every Tier 1 C++/Rust configuration.
- [ ] Tier 2 configurations compile and test where runners are available.
- [ ] Tier 3 failures are recorded but do not block Preview 0 unless promoted.
- [x] Every composed WGSL shader is validated during CI.
- [x] A minimal/headless startup test proves WGPU was actually selected.
- [x] `--render wgpu` cannot silently fall back to GL33 in tests.
- [x] Runtime fallback is explicit, logged, and visible.
- [x] Artifacts contain the executable, renderer library, shader hashes, and a build manifest.
- [x] Test output records commit, dirty-tree state, compiler, Rust toolchain, adapter, driver, backend, and enabled features.
- [x] One original mission loads in confirmed WGPU mode.
- [x] Missing optional packages do not prevent startup.
- [x] Deliberate GL33 fallback still works.

## CORE-NEG-002 — Versioned C++/Rust ABI — REQUIRED

- [x] ABI version query.
- [x] Renderer build ID.
- [x] Struct-size fields.
- [x] Struct-version fields where structures evolve.
- [x] Feature-bit negotiation.
- [x] Clear safe refusal for mismatched binaries.
- [x] Rust panic containment at every FFI entry point.
- [x] No C++ exception or Rust panic crosses the language boundary.
- [x] Test intentionally combines mismatched binaries and verifies a clear error.
- [x] ABI documentation identifies ownership and lifetime for pointers and handles.

Keep this proportional: do not build an elaborate plugin protocol unless the actual integration requires it.

## RND-005A — Renderer startup, capability, and safe shutdown — REQUIRED for Preview 0

- [x] Adapter and capability report.
- [x] Selected backend is queryable by tests and debug UI.
- [x] WGPU-active diagnostic or watermark state.
- [x] Explicit unsupported-feature fallback matrix.
- [x] Pipeline and shader creation errors identify the responsible resource/pass.
- [x] Resize and minimise do not corrupt the surface lifecycle.
- [x] Device loss is detected and reported gracefully.
- [x] Clean renderer shutdown.
- [x] Preview 0 does not require transparent in-process device recovery.

## RND-005B — Runtime renderer recovery and restart — REQUIRED for Milestone 1

- [ ] In-process device recreation where supported.
- [ ] Surface recreation after device failure.
- [ ] Renderer shutdown and restart stress test.
- [ ] Water/history/temporal resources restore or invalidate safely.
- [ ] Repeated restart cycles do not leak resources.
- [ ] Recovery failure produces a safe explicit fallback or controlled shutdown.

## CORE-005 — Authoritative machine-readable status ledger — REQUIRED

Store ticket truth in YAML, TOML, or JSON. Before this ticket is complete, the overlay bootstrap may create a deliberately minimal lock ledger containing only ticket ID, owner, branch, baseline commit, scheduling state, and blockers. That seed exists only to prevent duplicate claims and unsafe parallel work. `CORE-005` must migrate or validate it into the complete authoritative schema below without losing valid claims or silently overwriting live state.

A complete minimal record contains:

```yaml
id: EXAMPLE-001
is_example: true
title: Example ticket
class: RUNTIME_INFRA
obligation: REQUIRED
execution_mode: VALIDATE
scheduling_state: ACTIVE
status: VALIDATED
outcome: ADOPTED
owner: agent-name
branch: ticket/EXAMPLE-001
baseline_commit: abc123
verification_commit: fed987
milestone: 1
milestone_role: NON_BLOCKING_VALIDATION
depends_on: []
blocked_by: []
blocks: []
implementation_commits:
  - def456
production_call_sites:
  - file: engine/WgpuRenderer/Water.cpp
    symbol: WaterRenderer::UpdateSurfaceState
    verified_commit: fed987
test_ids:
  - water_pitch_sweep
benchmark_artifacts:
  - artifacts/example001.json
evidence_hash: sha256-example
decision_record: docs/decisions/EXAMPLE-001.md
known_failures: []
fallback: legacy_path
reviewer: independent-agent
supersedes:
  - docs/older-report.md#EXAMPLE-001
```

Lifecycle status vocabulary:

```text
PLANNED
RESEARCHED
PROTOTYPED
INTEGRATED
VALIDATED
SHIPPABLE
DEFERRED
SUPERSEDED
```

Scheduling state is tracked separately:

```text
OPEN
HOLD
BLOCKED
ACTIVE
DONE
```

Research outcomes:

```text
ADOPTED
ADAPTED
REJECTED
DEFERRED
NOT_APPLICABLE
```

- [x] “Completed” is not accepted as a standalone status.
- [ ] Every status names a baseline and verification commit.
- [x] Production call sites use file plus symbol, not fragile line numbers.
- [ ] Class, obligation, execution mode, scheduling state, lifecycle status, milestone, milestone role, and outcome are explicit where applicable.
- [ ] Every milestone-linked ticket declares `milestone_role`; CI treats omission as a schema error.
- [ ] Owner, branch, blocking relationships, evidence, and decision record are explicit.
- [ ] Old audits preserve historical truth but link to the superseding record.
- [x] CI validates schema, dependencies, and duplicate ticket IDs.
- [x] A `SHIPPABLE` ticket requires review by someone other than its implementation owner.
- [ ] Adopted external middleware records its upstream URL, version or commit, licence, maturity, known blockers, local patches, last review date, and reassessment triggers.
- [x] The repository contains a machine-readable Execution Overlay that references only canonical ticket IDs.
- [x] A bootstrap lock ledger can be created before full `CORE-005` completion and is promoted without losing valid claims.
- [ ] Overlay `initial_scheduling_state` and `initial_blocked_by` values seed missing records only and never overwrite live ledger state.
- [ ] Canonical milestone roles come from this roadmap and the validated ledger rather than from duplicated overlay metadata.
- [x] CI validates the overlay schema, WIP limits, referenced IDs, and conflicts between active tickets and dependency or hold states.
- [ ] The overlay is allowed to change without a master-roadmap version bump; role or constraint changes still require the appropriate decision record.
- [ ] Start with a simple ledger and overlay; generated dashboards may come later.

## REL-000 — Preview release package and public capability matrix — REQUIRED for public Preview 0

Activate only after the technical Preview 0 candidate satisfies the Build Truth tickets. The required outcome is a release that an external contributor can verify without reverse-engineering internal branches or reports.

- [ ] Versioned downloadable build or reproducible release artifact.
- [ ] Exact instructions for legally supplied original game data and supported startup configuration.
- [ ] Build fingerprint, renderer build ID, shader hashes, selected adapter/backend, and tested driver information.
- [ ] Named Tier 1 test configuration and any tested Tier 2 or experimental configurations.
- [ ] Concise “works / partial / unavailable / experimental” capability matrix derived from validated ledger records.
- [ ] Known limitations and fallback behaviour.
- [ ] At least one reproducible reference capture and timing bundle.
- [ ] GL33/WGPU comparison evidence where it explains compatibility or expected visual differences.
- [ ] A concise demonstration video or equivalent visual evidence showing the released build rather than a disconnected prototype.
- [ ] Independent-project disclaimer and the public project-identity wording from Section 1.
- [ ] Release notes distinguish validated production behaviour from future roadmap goals.
- [ ] Removing optional derived caches or packages restores the supported fallback without altering original data.

A stronger agent may improve the packaging, demonstration, or capability-report format, but must preserve reproducibility, honest scope, legal data handling, and externally verifiable evidence.

# 5. Foundation primitives

## CORE-000 — Repository and architecture audit — REQUIRED

- [x] Map renderer entry points and resource ownership.
- [x] Map C++/Rust/WGPU FFI and ABI assumptions.
- [x] Map terrain, weather, water, grass, smoke, physics, and Zeus ownership.
- [ ] Map current network authority and replication calls.
- [ ] Find direct reads bypassing canonical world APIs.
- [ ] Produce diagrams under `docs/architecture/`.

## TEST-001 — Harness kernel — REQUIRED

`TEST-001` provides harness-level scenario capture and restoration for repeatable testing. It does not define the authoritative multiplayer snapshot schema; that responsibility belongs to `NET-004`.

- [ ] Fixed simulation tick.
- [ ] Fixed random seeds.
- [ ] Deterministic event injection.
- [ ] Snapshot/restore.
- [ ] Camera-path recording/playback.
- [ ] Frame stepping.
- [ ] Deterministic time/weather controls.

## TEST-002 — Capture, metrics, and build fingerprint — REQUIRED

- [x] Screenshot capture.
- [x] Difference images with masks and tolerances.
- [x] CPU and GPU timing export.
- [x] Upload and draw/dispatch counters.
- [x] Build/driver/backend fingerprint.
- [x] Quality-profile automation.
- [x] Machine-readable result bundle.

Do not require bit-identical images across GPU vendors. Gameplay-state checksums should be exact or explicitly quantised; rendering comparisons use approved tolerances and masks.

## TEST-003 — Rendering reference scene pack — REQUIRED

- [ ] Ocean shore and aerial.
- [ ] Rocky coast and beach.
- [ ] River and lake.
- [ ] Calm and windy meadow.
- [ ] Forest ground/helicopter/aircraft.
- [ ] Helicopter downwash.
- [ ] Windowed and sealed interiors.
- [ ] Corridor smoke.
- [ ] Artillery crater.
- [ ] Zeus stress scene.

## TEST-004 — Dedicated server and two-client pack — REQUIRED

- [ ] Start dedicated server.
- [ ] Join two clients.
- [ ] Apply authoritative events.
- [ ] Compare gameplay checksums.
- [ ] Test reconnect and late join.
- [ ] Run clients at different rendering frame rates and quality settings.

## TEST-005 — Fault injection — REQUIRED

- [ ] Latency.
- [ ] Jitter.
- [ ] Packet loss.
- [ ] Duplication.
- [ ] Reordering.
- [ ] Temporary disconnection.
- [ ] Recovery and resynchronisation.

## TEST-006 — GL33/WGPU comparison — REQUIRED where applicable

- [x] Same mission and camera.
- [x] Confirm selected backend.
- [x] Record expected visual differences.
- [x] Detect accidental WGPU fallback.
- [x] Verify original-data compatibility.

## NET-001 — Canonical authoritative tick identity and ordering — REQUIRED

The initial goal is a canonical authoritative tick identifier and explicit ordering for new replicated systems. It does **not** require converting all legacy gameplay into deterministic lockstep or replacing every timing path immediately.

- [ ] Introduce `SimulationTick` for new authoritative events and state.
- [ ] Tick-stamp weather transitions, terrain edits, water impulses, Zeus commands, smoke gameplay events, and new projectiles where applicable.
- [ ] Define ordering among new authoritative event classes.
- [ ] Define interpolation between authoritative states.
- [ ] Bridge legacy timing paths explicitly where deeper migration is not yet justified.
- [ ] Define pause, slow motion, and acceleration semantics for new systems.
- [ ] Record deeper legacy migration as separate evidence-driven tickets.

## NET-002 — Stable identity allocation — REQUIRED

Stable IDs for entities, bodies, projectiles, terrain edits, water impulses, smoke emitters, weather transitions, Zeus commands, transactions, and simulation domains.

## NET-003 — Versioned idempotent event journal — REQUIRED

- [ ] Ordered.
- [ ] Versioned.
- [ ] Safe to receive twice.
- [ ] Compactable.
- [ ] Replayable.
- [ ] Schema migration defined.

## NET-004 — Authoritative network snapshots, reconstruction, and checksums — REQUIRED

This ticket defines gameplay-authoritative network and late-join reconstruction. It may reuse harness utilities from `TEST-001`, but must not duplicate a separate incompatible snapshot model.

- [ ] `SerializeSnapshot`.
- [ ] `ApplySnapshot`.
- [ ] `ReplayEvents`.
- [ ] `ComputeChecksum`.
- [ ] Revision APIs.
- [ ] Late-join reconstruction.
- [ ] Developer divergence display.

## NET-005 — Fault and late-join harness — REQUIRED

Use TEST-004 and TEST-005 to validate every authoritative subsystem.

## NET-006 — Save/network schema migration — REQUIRED

- [ ] Every persistent schema is versioned.
- [ ] Forward/backward compatibility policy documented.
- [ ] Unsupported state fails safely.
- [ ] Migration tests exist.

## WORLD-001 — Introduce canonical TerrainQuery — REQUIRED

- [ ] Define terrain-height and surface-material queries.
- [ ] Initially delegate to current immutable terrain behaviour.
- [ ] Instrument direct-read bypasses.
- [ ] Prove baseline behaviour remains identical.
- [ ] Do not begin deformation until `TRN-GATE-1` passes.

## WORLD-002 — Introduce canonical WaterQuery — REQUIRED

- [ ] Define height, depth, normal, surface velocity, foam, turbulence, and body identity.
- [ ] Initially delegate to current water/ocean behaviour.
- [ ] Instrument bypassing water reads.
- [ ] Prove baseline behaviour remains identical.
- [ ] Do not add broad new backends until `WTR-GATE-2` passes.

## WORLD-003 — Introduce canonical Weather/AtmosphereQuery — REQUIRED

- [ ] Define authoritative weather-state access.
- [ ] Define deterministic wind and atmosphere sampling interfaces.
- [ ] Initially delegate to current weather and grass-wind behaviour.
- [ ] Instrument bypassing consumers.
- [ ] Prove current grass gust behaviour remains unchanged.
- [ ] Do not expand the shared field until `ATM-GATE-1` passes.

## ZEU-000A — Thin authoritative command substrate — REQUIRED early

- [ ] Versioned command envelope.
- [ ] Stable command ID.
- [ ] Permission check.
- [ ] Server validation/execution.
- [ ] Transaction grouping.
- [ ] Undo metadata.
- [ ] Event-journal integration.
- [ ] Existing ImGui adapter.

## CORE-006 — Agent execution and integration protocol — REQUIRED

- [ ] One active owner per implementation ticket.
- [ ] One branch or worktree per implementation ticket.
- [ ] Ledger records owner, branch, baseline commit, and integration target.
- [ ] Shared ABI, schema, resource-layout, or serialisation changes have an integration owner.
- [ ] Agents do not silently modify unrelated systems.
- [ ] Generated files and benchmark artifacts are distinguished from authored source.
- [ ] Every handoff lists changed paths, production call sites, tests, measurements, risks, compatibility impact, and remaining work.
- [ ] Independent audit is performed by a different agent.
- [ ] Self-authored work cannot self-approve `SHIPPABLE`.

## CORE-007 — Reference-project code provenance — REQUIRED

`ASSET-010` governs third-party ASSETS. Nothing governed third-party CODE, and reference projects
are now being read for ideas and implementations (`inspiration/`), so the same discipline applies.

**The trap this exists to prevent.** "Esoterica is MIT" is true of Esoterica's own files and false of
a large part of its tree: it bundles EA (EASTL), Dear ImGui, implot, stb, pugixml, rpmalloc, mpack,
concurrentqueue, EnkiTS, pcg, lzav, mINI, D3D12MemoryAllocator, meshoptimizer and Box3D, each under
its own terms. A file copied from `Code/Base/ThirdParty/` is not covered by the project's own
licence, and the mistake is invisible in review because the repository root says MIT.

Before any code is copied or closely adapted from a reference project:

- [ ] Record the project, its licence, the exact commit, and the file paths taken.
- [ ] Confirm each file is authored by that project rather than bundled from a third party; where it
      is bundled, record THAT component's licence and treat it as its own decision.
- [ ] Retain the required copyright and permission notices, and add the entry to
      `THIRD_PARTY_NOTICES.md`.
- [ ] Name the Poseidon ticket and the real consumer the code is for. Reference material adopted
      without a consumer becomes unowned code nobody can safely delete later.
- [ ] Prefer adapting the design to Poseidon's own boundaries over transplanting files. A pattern
      carries no licence obligation; a file does.

Reading a reference project for ideas needs none of this. It applies at the point code is copied.

### Reference projects — what each is good for

Recorded so the next agent does not re-derive it, and does not reach for the wrong one:

- **Esoterica** (MIT, `inspiration/Esoterica-main`) — a compact custom-engine starter framework. It has
  NO grass or vegetation system -- checked, the only matches are an icon-font name and a Box3D
  internal file -- so do not send anyone there for GRS work. The better CLOSE-UP reference: GPU-driven rendering structure, instance and cluster culling, animation
  runtime internals, ragdoll and hitbox tooling, property grids, timelines and node graphs, resource
  hot-reload. Its authors describe it as a prototype rather than production software, and its
  renderer is DX12-oriented (it bundles D3D12MemoryAllocator and ships no Vulkan backend), so it
  informs the WGPU path rather than supplying it.
- **O3DE** (Apache-2.0) — the better WIDE-ANGLE reference: what a mature asset pipeline, scene
  import, dependency tracking and animation authoring workflow must eventually cover. Too
  framework-coupled to transplant.

Neither is a blueprint. Poseidon keeps its own renderer, entity model, mission and network
semantics; an engine-wide ECS or renderer transplant would break save, mission and multiplayer
compatibility for no visible gain.
- [ ] Conflicting tickets are blocked through ledger dependencies.
- [ ] Agents stop and rebase or revalidate when their baseline is no longer an ancestor of the integration branch.
- [ ] Architecture/schema merge conflicts trigger renewed validation.
- [ ] Integration owner records final verification commit and evidence hash.
- [ ] Only one active ABI or shared-schema change is allowed per integration target unless the integration owner explicitly approves coordination.
- [ ] Only one active canonical-query migration is allowed per subsystem.
- [ ] Quick Wins touching an actively refactored subsystem are blocked.
- [ ] The integration owner sets a maximum number of concurrent large cross-system tickets.
- [ ] Long-running branches periodically rebase and revalidate.


## DBG-REC-000 — Reproducible flight recorder and bug bundle — SUGGESTED early capability

**Required question:** Can crashes, desynchronisation, rendering regressions, and difficult gameplay failures be reproduced from evidence rather than anecdotal reports?

The preferred direction is a bounded rolling recorder that can export a compact replay and diagnostic bundle. A single monolithic recorder is not mandatory; agents may reuse the event journal, platform crash facilities, renderer captures, or specialised subsystem traces when that is more reliable.

The initial slice should remain deliberately small: build and backend fingerprints, recent logs, adapter/driver data, screenshot, recent timings, quality settings, relevant event identifiers already available, and a crash dump where the platform provides one. It must not wait for a full deterministic replay model, final authoritative snapshots, or cross-version replay migration.

Candidate evidence:

- Recent authoritative events, player inputs, and Zeus transactions.
- Snapshot revisions, checksums, and resynchronisation history.
- Camera transforms and deterministic test controls.
- Renderer backend, quality profile, resource failures, GPU markers, and recent pass timings.
- Build, shader, asset-package, adapter, driver, and schema fingerprints.
- Recent warnings, network-fault settings, and relevant save or regional snapshot state.
- Screenshot, crash dump, and minimal reproduction manifest where available.

Acceptance principles:

- [ ] Recording overhead and retention are bounded.
- [ ] Sensitive data and unnecessary user content are excluded or explicitly opt-in.
- [ ] The bundle identifies unavailable evidence instead of inventing it.
- [ ] A captured failure can be replayed or narrowed more effectively than from logs alone.
- [ ] The design composes with `NET-003`, `NET-004`, `TEST-001`, and `TEST-002` rather than creating incompatible replay formats.
- [ ] A simpler specialised solution may be accepted when it produces equivalent or better reproducibility.

## WORLD-004 — Spatial change and invalidation contract — SUGGESTED shared outcome

Terrain edits, vegetation changes, water changes, asset reloads, destruction, and Zeus transactions can invalidate many derived systems. The required outcome is consistent, revision-aware propagation of spatial changes; one universal event bus or dirty-region manager is only a suggested mechanism.

Candidate change record fields:

```text
Authoritative revision
Affected region, tiles, or object set
Semantic change type
Source transaction or event
Persistent versus temporary state
Urgency and scheduling hints
Expected consumers
```

Potential consumers include rendering, collision, navigation, vegetation, water bathymetry, surface state, lighting data, audio propagation, far-world representations, streaming caches, and save/late-join state.

- [ ] Consumers cannot silently remain stale after authoritative changes.
- [ ] Nearby changes can be coalesced without losing required semantics.
- [ ] Expensive rebuilding can be budgeted across frames where gameplay permits.
- [ ] Debug tools expose dirty regions, pending consumers, revisions, failures, and estimated rebuild cost.
- [ ] Specialised direct invalidation remains acceptable when it is clearer or more efficient than a shared service.
- [ ] This ticket must not create a universal abstraction before at least two real consumers prove the need.


## RND-000 — Renderer baseline and budgets — REQUIRED

For every reference scene record:

- [ ] CPU frame time.
- [ ] GPU pass times.
- [ ] Draw and dispatch counts.
- [ ] Upload bytes.
- [ ] `write_buffer`/copy/barrier counts where available.
- [ ] Bind-group and pipeline changes.
- [ ] Transient and persistent VRAM.
- [ ] Pipeline creation count.
- [ ] Texture residency.
- [ ] Readbacks and synchronisation stalls.
- [ ] Warm-up duration.
- [ ] Capture duration or frame count.
- [ ] Median frame time.
- [ ] 95th and 99th percentile frame time.
- [ ] 1% low frame rate.
- [ ] Maximum CPU and GPU stall.
- [ ] Shader/pipeline compilation spikes.
- [ ] VSync and frame-limiter state.
- [ ] Warm-cache and cold-cache conditions.
- [ ] Clock and power state where measurable.

Define provisional budgets by hardware tier and quality profile. A change that improves the mean but introduces visible long stalls is not automatically a performance improvement. Every major renderer feature declares its permitted share.

## RND-010 — Pass/resource registry — REQUIRED

Keep it lightweight initially. Track producer, consumers, format, dimensions, lifetime, history owner, update cadence, invalidation, timing, and debug view. Do not begin a full render-graph rewrite unless measurements justify it.

## QW-000 — Controlled Quick Wins lane — REQUIRED governance, optional contents

This lane begins **after Preview 0** and may run in parallel with later milestones. It does not bypass Build Truth.

A candidate qualifies only when it is:

- Small and reversible.
- Attached to an existing production path.
- Clearly beneficial.
- Measurable before and after.
- Free of new persistent schemas, ABI changes, networking models, or major abstractions.
- Unlikely to conflict with an active architectural ticket.
- Reviewable in one focused cycle.
- Protected by a fallback or easy revert.

Each candidate still requires:

- [ ] Owner and baseline commit.
- [ ] Production call-site verification.
- [ ] Before/after evidence.
- [ ] Applicable tests.
- [ ] Performance measurement where relevant.
- [ ] Independent review.
- [ ] Ledger update.

Suggested candidates for agents to evaluate:

- Make active WGPU/GL33 backend and fallback reason unmistakable.
- Fix confirmed dead, disconnected, or stale water/grass parameter paths.
- Improve shader, pipeline, resource, and lifecycle errors.
- Add safe missing-texture or optional-content fallback.
- Remove measured upload or state-change overhead without changing architecture.
- Add useful debug views and capture metadata.
- Connect one already-supported external grass texture to the existing production path behind a reversible fallback, only when no new package, material, ABI, or streaming architecture is required.
- Fix small GL33 correctness issues while preserving fallback.

Not Quick Wins:

- Unified WaterWorld.
- New volumetric-cloud architecture.
- Terrain deformation.
- Broad physics replacement.
- Whole-engine deterministic conversion.
- Voxel far landscape.
- A new render graph.
- Any “small prototype” that introduces permanent architecture before its decision gate.


---

# 6. Renderer stabilisation and ocean closure

> [!WARNING]
> **`WTR-` identifiers are ambiguous in this repository.** Two independent
> numbering schemes exist, and six identifiers name different things in each:
>
> | ID | This roadmap (**authoritative for tickets**) | `.agents/CWR-CE Water System Master Plan.md` (design phases) |
> | --- | --- | --- |
> | `WTR-001` | Close current water integration gaps | Deterministic water debug mode |
> | `WTR-100` | WaterBody registry and backend expansion | Underwater classification and waterline |
> | `WTR-110` | Generalise current FFT | Underwater optics |
> | `WTR-120` | Anti-repetition | Underwater god rays |
> | `WTR-130` | Persistent foam | Surface-derived caustics |
> | `WTR-140` | Reflection ownership | Underwater bubbles and local aeration |
>
> **`WTR-` tags in source code refer to the Master Plan, not to this roadmap.**
> Tags such as `WTR-031` and `WTR-085` have no ticket here at all.
>
> Always state which document you mean. New water tickets must take identifiers
> free in **both** schemes — see
> [`WTR-external-proposal-review-20260802.md`](../decisions/WTR-external-proposal-review-20260802.md),
> which records how an external proposal landed on occupied identifiers because
> of this.

## WTR-001 — Close current water integration gaps — REQUIRED

Agents must re-audit the current branch and update the status ledger.

- [x] Verify `WaterSurfaceState` reaches every intended consumer. **It does not.** Audited 2026-08-02 against `water/water.wgsl`: six of its nineteen fields are written and never read back — `material_position`, `displaced_pos`, `previous_displaced_pos`, `jacobian`, `interaction_height`, and `breaking_energy`, the last of which is neither written nor read (a placeholder for `WTR-080`). `jacobian` is set to a constant `1.0` at line 1226 and never consulted; the breaking logic derives `jacobian_break` from `compression` instead.
- [x] Verify previous displacement and velocity persist and are consumed. **Split result:** `velocity` is consumed; `previous_displaced_pos` is computed (`world_pos - velocity * 0.0333`, line 1218) and then **never read by anything**. Reprojection currently has no consumer.
- [x] Remove or integrate zero-call-site helpers. None found at the FFI boundary: all 49 `wgr_*` exports have C++ call sites, and `cargo clippy` reports no dead code in the renderer crate (`pub extern` items are invisible to `dead_code`, so they were checked explicitly). The dead *fields* above are the equivalent finding one level down. **Recorded, not deleted** — removing them is a shader change whose value depends on whether `WTR-080` is imminent, which is a water-owner call.
- [x] Separate geometry, normal, foam, and roughness cascade weights. **Three of four done.** `CascadeWeights` in `water/water.wgsl` carries `geometry_weight`, `normal_weight` and `foam_weight`, each from its own `smoothstep` on projected pixel footprint (thresholds 1.5–4.0, 0.5–2.0, 1.0–3.0 respectively). There is **no `roughness_weight`**: roughness is derived separately in `water_roughness()` from `slope_variance`. This supersedes `docs/wtr-audit-report.md`'s WTR-032 entry ("single fade per cascade used across both geometry and normals"), which was accurate on 2026-07-25 and has since been implemented.
- [x] Verify slope variance and roughness compensation. **The double-count is real and still present**, re-confirming `docs/wtr-audit-report.md`'s WTR-033 entry rather than superseding it. `water_roughness(spec_power, lost_variance, ...)` names its second parameter for the *unresolved* variance and adds `micro_slope * 0.35 + lost_roughness * 0.45`. But `micro_slope` is measured from the shading normal, which `fft_normal_with_weights` already built from the cascade weights — so it represents the *resolved* slope — while the value passed as `lost_variance` is `state.slope_variance`, accumulated in `water.wgsl` as an unweighted `fft_slope_var += aux.w` over every enabled cascade, i.e. the **total**. The resolved portion is therefore counted twice, and water reads rougher than the spectrum implies, most visibly where cascade weights are high. The residual would be the same accumulation weighted by `(1 - normal_weight)` per cascade. **Fixed** (2026-08-03): a second accumulator carries `aux.w * (1 - normal_weight)` and `water_roughness` consumes that instead of the total, leaving `slope_variance` untouched for its three other consumers (`micro_normal`'s detail fade, `open_ocean_activity`, debug view 4). Cost is unchanged — 0.585 ms before vs 0.589 ms after at the beach viewpoint, well inside the ±0.45 ms noise floor, with identical node and triangle counts. **No visible difference was demonstrated**: the two captures were not frame-matched (the FFT keeps advancing between shots) and the viewpoint is close-up water, where `normal_weight` is high and the residual is therefore near zero. Kept on Oliver's decision as the arithmetically correct behaviour at no cost, not on evidence that it looks better. A frame-frozen A/B via `WGR_WATER_FREEZE_FFT` would settle that.
- [x] Add contribution debug views. Present and labelled in `water/water.wgsl`'s `debug_view()`: directional sky (39), directional clouds (40), planar sky (41), planar clouds (42), planar terrain/objects (43), planar geometry validity (44), SSR (45), and a final reflection-owner badge (46). Note `docs/wtr-040-reflection-ownership-report.md` numbers these **37–44**; they are now **39–46**, so following that report selects the wrong channels — 37 and 38 are WTR-012 views. Corrected in place there.
- [ ] Resolve reflection ownership. The debug channels exist, but whether cloud duplication is actually gone is a visual judgement no code inspection can settle, and `docs/wtr-audit-report.md` still records it as outstanding. Needs a look through views 40 and 42.
- [ ] **Defect found while auditing the above:** debug view 38, labelled "WTR-012 Previous displacement delta", does not show previous displacement. It computes `abs(interaction.y * 0.0333)` — the interaction field, borrowing the same timestep constant, so the output looks plausible. Anyone using it to confirm previous displacement works would be misled; the field is still written once and read by nothing.
- [x] Add SSR confidence. Implemented and load-bearing, not just present. The SSR trace returns confidence in its alpha as `hit_weight * roughness_fade * distance_fade` — screen-edge proximity, a fade above `normal_variation` 0.08–0.30 so rough water stops trusting the trace, and a ray-distance fade over 120–192 m — then `water.wgsl` scales it again by a world-distance weight over 180–320 m. It arbitrates reflection ownership rather than merely tinting: SSR is mixed in by `ssr.a`, and the planar reflection is attenuated by `(1 - ssr.a * 0.80)`, so higher SSR confidence directly yields the planar contribution. Debug view 21 exposes the channel on its own.
- [ ] Test pitch stability, aerial repetition, history invalidation, and lifecycle reset.
- [ ] Reconcile old audit reports and mark superseded findings.

## WTR-GATE-1 — Ocean production closure — REQUIRED before unified water

- [x] Every claimed ocean feature has a production call-site audit. Covered by the `WTR-001` audit (all 49 `wgr_*` FFI exports have C++ call sites; six `WaterSurfaceState` fields are written and never read; `world_to_material_pos` has one caller; debug view 38 reports a different quantity than its label) and by [`engine/WgpuRenderer/docs/env-switches.md`](../../../engine/WgpuRenderer/docs/env-switches.md), which inventories all 30 `WGR_*` switches and records that eight of them were inert before `9fb2e47`. **Any earlier report claiming a feature was verified via one of those eight should be distrusted** — before that fix the switch could not have taken effect.
- [ ] Visual captures verify reflection ownership.
- [ ] Aerial and pitch tests pass or accepted limitations are recorded.
- [x] Performance baseline exists. [`docs/roadmap/evidence/water-baseline-20260802.md`](../evidence/water-baseline-20260802.md) — WGPU on an RTX 3070, Malden ground level, steady-state: **~3.2 ms** water GPU total, 28 CDLOD nodes, 516,096 triangles, `lod0=27 lod1=1`. Sample spread is ±0.45 ms, so **sub-15% changes at this viewpoint are not distinguishable from noise**. It is one viewpoint and a regression anchor, not a characterisation of water cost across the game — extend with aerial and coastal before generalising.
- [x] Water can be disabled independently — **it could not be until now**. `Landscape::DrawWater` already falls through to the legacy per-segment mesh when `GetWaterRenderer()` is null, and `EngineWgpu` gates construction on `WGR_GPU_WATER`, but `ConfigureWgpuUltraEnvironment()` overwrote that variable (and `WGR_HDR`, `WGR_MSAA`, `WGR_PREPASS`, `WGR_INDIRECT`, `WGR_GPU_DRIVEN`, `WGR_WATER_FFT`, `WGR_SHADOW_MAPS`) unconditionally at startup, before renderer creation read them. **Every documented `WGR_*` switch was silently ignored**, which also made the dev panel's own hint ("run the wgpu backend with `WGR_GPU_WATER`") impossible to act on. Those are now applied as defaults, so an explicit override wins. Verified all three ways: `0` → no water draws, `1` → water draws, unset → water draws, so the shipped profile is unchanged.
- [ ] Contradictory status reports are resolved in CORE-005.

## GL33-010 — Legacy shadow-filter maintenance — OPTIONAL

- [ ] Verify grass light upload and sign.
- [ ] Compare equal filter radius and bias.
- [ ] Measure movement shimmer.
- [ ] Add old/new toggle.
- [ ] Compare stable Poisson/world-space alternatives.
- [ ] Preserve original fallback.
- [ ] Do not make WGPU depend on this path.


## TEST-GRS-001 — Grass baseline capture set — REQUIRED for Milestone 1 exit

- [ ] Calm and strong-wind meadow.
- [ ] Ground, vehicle, helicopter, and aircraft camera paths.
- [ ] Helicopter downwash and shadow deformation.
- [ ] CPU, GPU, overdraw, and temporal-stability evidence.
- [ ] GL33/WGPU comparison where applicable.

## TEST-ZEU-001 — Zeus baseline and stress tests — REQUIRED for Milestone 1 exit

- [ ] Existing selection, spawn, move, rotate, copy, paste, and delete paths.
- [ ] Network-aware creation and movement.
- [ ] Stress scene with many editable entities.
- [ ] Current limitations recorded before backend extraction.
- [ ] Regression captures and logs.

## RND-020 — Renderer-overhead investigation — INVESTIGATE

- [ ] Use `RND-000` evidence to identify dominant upload, copy, barrier, bind, pipeline, or synchronisation overhead.
- [ ] Define a bounded question and baseline.
- [ ] Compare candidate optimisations.
- [ ] Measure median and tail latency.
- [ ] Record rejected approaches as valid outcomes.
- [ ] Produce one of: `ADOPTED`, `ADAPTED`, `REJECTED`, or `DEFERRED`.
- [ ] Do not silently transition into implementation.

## RND-021 — Implement approved renderer-overhead optimisation — IMPLEMENT

Depends on an `ADOPTED` or `ADAPTED` outcome from `RND-020`.

- [ ] Implement only the selected change.
- [ ] Preserve a fallback or easy revert path.
- [ ] Record before/after CPU, GPU, upload, and tail-latency evidence.
- [ ] Verify no unrelated architectural changes were introduced.
- [ ] Independent audit by an agent other than the implementer.


## RND-030 — Existing renderer-plan reconciliation — REQUIRED validation before overlapping renderer work

The repository may already contain implementation roadmaps, audits, branches, prototypes, and partial production paths for depth prepass, GPU culling, multi-view rendering, bindless resources, lighting, shadows, HDR, skinning, and related renderer work. Before starting an overlapping system:

- [x] Inventory relevant renderer plans and their baseline commits.
- [x] Map their ticket or phase names into the authoritative ledger.
- [x] Verify claimed production call sites in the active integration branch.
- [x] Separate implemented, integrated, validated, superseded, experimental, and documentation-only work.
- [x] Record dependencies on water, materials, atmosphere, shadows, far-world rendering, and streaming.
- [x] Mark contradictory or outdated reports as historical rather than silently deleting them.
- [x] Reuse strong existing work; do not implement a second system merely because the master roadmap uses different terminology.
- [ ] A stronger agent may consolidate or replace old plans when the decision record demonstrates lower complexity, better portability, or better measured performance.

**State (2026-08-03).** The inventory, verification and dependency map live in
[`RND-030-renderer-plan-reconciliation-20260802.md`](../decisions/RND-030-renderer-plan-reconciliation-20260802.md).
All seven plans whose status line disagreed with the branch now carry an in-place reconciliation
note; none was deleted.

Re-auditing the shadow row while closing that out found the report had overstated it:
`cascaded-shadow-map-plan` was filed under "actually live" on the strength of
`MAX_CASCADES = 4`, which is the value its Tier 1 exists to change to 8. Tier 1 is two of four
items. The correction is recorded in the report and in the plan. Read the rest of that matrix
as "a symbol the plan mentions exists in the branch", not "the plan landed".

**Ledger mapping is done** — [`renderer-systems-ledger.yaml`](../renderer-systems-ledger.yaml)
registers the twelve renderer systems that ship, each with an id, an owner slot, a plan
reference and verified production call sites.

It is a separate file on purpose. `status-ledger.yaml` is the Preview-0 execution ledger and
its validator requires `authorised_tickets == execution_queue == ledger ids`, so putting these
entries there would assert they are Preview-0 scope, which they are not.

`scripts/validate_renderer_ledger.py` re-checks every recorded call site against the branch —
tracked file, symbol still present — and runs in the Preview-0 ledger CI job. That check is the
point rather than a formality: this report cited `MAX_CASCADES = 4` as proof the shadow plan had
shipped, when 4 is the value its Tier 1 exists to change, and a register without mechanical
verification would drift the same way. The distinction it enforces is that a verified call site
proves a system **exists**, which is weaker than a plan having landed; per-system `completeness`
carries that judgement and records which entries were re-audited rather than carried over.

**Reuse / consolidation is done** —
[`RND-030-renderer-consolidation-20260803.md`](../decisions/RND-030-renderer-consolidation-20260803.md),
written off the completed audit rather than off the plans.

Its headline: **there is no consolidation win, because there is nothing to consolidate.** No
system is implemented twice, and only one built system sits unused — the compute skin bake, off
deliberately because standalone it is pure overhead, and waiting on GPU-culling stage 6.

The branch is in materially better shape than its own plan documents say, and the drift runs
*pessimistic*: status blocks under-report what shipped. Anyone planning from the plans alone
will re-implement things that already run. The record's §2 sets out the trap — "on by default"
is decided in three layers (the Rust default, the C++ default, and
`ConfigureWgpuUltraEnvironment` setting the environment variables themselves before the engine
exists), and the layer furthest from the renderer is the one that decides the shipped game.

No duplicated systems were found, which was the specific risk this box guarded against. The
plans overlap in ambition but not in code — the skin-bake plan explicitly refuses to build a
draw coalescer because the GPU-culling indirect path already is one, and that refusal is the
pattern to copy.

The four genuinely unbuilt candidates, if renderer work is picked up next: `forward-plus`,
`screen-space-ao`, `gpu-terrain-water-cull`, `terrain-fractal-detail`.

## FRAME-000 — Frame pacing, latency, and simulation/render decoupling — REQUIRED measurement outcome, implementation INVESTIGATE

Average frame rate is not sufficient evidence of responsiveness or stability. The required outcome is a measurable account of where latency and uneven frame delivery originate, plus bounded improvements where the current behaviour is unacceptable. A new threading model, frame graph, low-latency API, or render/simulation architecture is not required unless evidence supports it.

Questions to answer:

- How much time passes from sampled input to visible presentation on each Tier 1 configuration?
- Where do simulation jitter, render-thread stalls, present blocking, queue depth, shader compilation, streaming, readback, and device synchronisation enter the frame?
- Which latency is inherent to the existing game loop, and which is introduced by WGPU integration or new systems?
- Can simulation, rendering, interpolation, and presentation be improved incrementally without changing gameplay outcomes or multiplayer authority?

Candidate evidence and tools:

- CPU/GPU frame timeline with simulation, render submission, queue, and present markers.
- Input-sampling timestamps and camera-response tests.
- Present mode, VSync, variable-refresh, frame limiter, and queue-depth captures.
- Stutter attribution for shader/pipeline compilation, allocation, upload, streaming, and device recovery.
- Fixed-camera and interactive-camera tests at median, p95, p99, and worst-frame latency.
- Comparison of GL33 and WGPU where it identifies integration-specific regressions.

- [ ] Do not declare success from average FPS alone.
- [ ] Improvements must preserve authoritative simulation semantics and avoid quality-dependent gameplay changes.
- [ ] Agents may recommend interpolation, scheduling, queue-depth limits, pipeline caching, async work, or another mechanism when evidence supports it.
- [ ] A whole-engine threading rewrite is out of scope unless smaller changes cannot meet the accepted target.
- [ ] Findings may promote specific fixes to blocking only when a measured release problem cannot be resolved through an approved fallback.

## TEMP-000 — Temporal-history consistency and motion-data contract — REQUIRED outcome, implementation INVESTIGATE

Multiple systems may use temporal history, including reflections, clouds, volumetrics, occlusion, water, foam, post-processing, vegetation, and future reconstruction or upscaling. The required outcome is coherent temporal behaviour and safe invalidation; a single global temporal framework is only one candidate design.

Questions to answer:

- Which systems genuinely need shared motion, jitter, depth, confidence, or reactive data?
- Which systems are better served by specialised local histories?
- What must happen on camera cuts, teleports, origin changes, resize, device recovery, quality changes, backend changes, and dynamic-resolution changes?
- How are rigid, skinned, procedural, alpha-tested, transparent, particle, water, and vegetation motion represented or deliberately approximated?

Candidate capabilities:

- Previous/current camera and object transforms.
- Stable jitter sequencing where used.
- Motion vectors or equivalent reprojection data.
- Disocclusion, confidence, transparency, and reactive masks.
- History identity, generation, age, reset reason, and validity.
- Debug views for motion, rejection, confidence, and history age.

- [ ] Do not require TAA, temporal upscaling, or one vendor technique as the only solution.
- [ ] Native-resolution, MSAA/alpha-to-coverage, temporal, supersampling, and screenshot paths may coexist when justified.
- [ ] A shared framework must prove that it reduces duplication or inconsistency rather than centralising unrelated histories.
- [ ] Temporal techniques must be measured for ghosting, shimmer, disocclusion failure, vegetation behaviour, water behaviour, latency, and platform cost.
- [ ] If a shared framework is rejected, the decision must still define common lifecycle and invalidation rules.

## COORD-000 — Large-world precision and render-relative coordinates — INVESTIGATE

Before greatly extending view distance, aircraft visibility, far-world rendering, water scale, or procedural atmosphere, evaluate precision at map centre and extreme world coordinates.

Required evidence should cover:

- Camera and object jitter.
- Terrain seams and depth precision.
- Shadow, reflection, cloud, water-phase, grass-noise, and particle stability.
- Physics/render disagreement.
- Secondary-view behaviour.
- Save, replay, multiplayer, and Zeus-teleport implications.

Candidate mechanisms include camera-relative rendering, regional anchors, high/low coordinate splits, selective double precision, stable procedural coordinate domains, or targeted fixes to existing transforms.

- [ ] Authoritative gameplay coordinates must not be changed merely to simplify rendering.
- [ ] Visual origin handling must not alter network identity or deterministic gameplay state.
- [ ] Do not introduce global origin rebasing unless a bounded investigation proves it superior to a rendering-boundary conversion.
- [ ] Findings become required inputs to `FAR-000` and any world-streaming decision.
- [ ] A stronger alternative is welcome when it provides better stability with lower integration risk.

# 7. Materials, normal maps, and HD packages

## AST-005 — Early asset-compatibility spine — REQUIRED before material and grass expansion

This is not the full Arma compatibility programme. It establishes the common interpretation rules needed by materials, HD packages, grass, and later importers.

- [ ] Audit current PAA, P3D, RVMAT, WRP, and config readers.
- [ ] Define canonical case-insensitive asset identity and path handling.
- [ ] Detect files or package entries differing only by case.
- [ ] Never resolve case-only collisions silently.
- [ ] Report every package supplying a colliding candidate.
- [ ] Apply deterministic precedence only after validation.
- [ ] Define visual hashes separately from gameplay-critical hashes.
- [ ] Define cache versions and invalidation rules.
- [ ] Define parser failure behaviour for malformed input.
- [ ] Add path-traversal and archive-safety tests.
- [ ] Ensure an optional overlay cannot shadow unrelated mod content accidentally.
- [ ] Route new material and texture work through the audited `PoseidonFormats` stack unless evidence supports a replacement.

## AST-006 — Cross-generation compatibility fixture proof — REQUIRED for Compatibility Preview C0

This is a bounded compatibility proof, not a claim of full Arma 1, Arma 2/OA, or Arma 3 gameplay compatibility. Use synthetic, openly licensed, legally redistributable, or locally supplied fixtures. Proprietary game data must not be committed unlawfully.

The initial fixture set should exercise representative generations and capabilities, for example:

- An OFP/CWA baseline fixture proving unchanged legacy behaviour.
- A legally usable static fixture representative of an Arma 1-era format path.
- A legally usable animated fixture representative of an Arma 2/OA-era path.
- A legally usable static/material fixture representative of a later Arma 3-era path.

These examples are not mandatory file selections. Stronger agents may substitute a smaller or more representative synthetic corpus when it exercises the relevant versioning, geometry, material, animation, and unsupported-feature paths more clearly.

Required outcome:

- [ ] Fixture provenance, licence, source hash, and intended coverage are recorded.
- [ ] Format generation and version are detected explicitly rather than guessed from filenames alone.
- [ ] Conversion or cache generation is deterministic and versioned.
- [ ] Unsupported, partially supported, and lossy features produce structured reports.
- [ ] Supported static geometry and materials render in a reference scene.
- [ ] The activated animated fixture either plays through the supported path or produces a precise unsupported-feature result.
- [ ] Original files remain immutable.
- [ ] Derived caches can be deleted and rebuilt safely.
- [ ] Missing later-generation fixtures or unsupported features do not prevent normal OFP/CWA startup.
- [ ] A false-positive import is treated as worse than an explicit, actionable rejection.
- [ ] The resulting compatibility matrix distinguishes static visuals, animation, complete simulation, and mission/addon behaviour.

Compatibility Preview C0 succeeds when the engine proves a trustworthy, reversible, diagnosable path across selected generations. It does not require every fixture to be fully supported.

## MAT-000 — Legacy and modern materials — REQUIRED

Accept both:

- Legacy Real Virtuality/RVMAT-style materials.
- Native PBR metallic/roughness materials.
- Specialised vegetation, glass, water, and emissive materials.

Do not permanently reduce all new PBR assets to the old Phong-style model. Preserve a genuine legacy path for older content and offer optional conversion tools.

Checklist:

- [ ] Explicit internal material model.
- [ ] Native legacy rendering where required.
- [ ] Native PBR path.
- [ ] Filename suffix recognition for old assets.
- [ ] Explicit material manifests for new assets.
- [ ] Reference-sphere tests for metals, dielectrics, vegetation, glass, and wet surfaces.
- [ ] Shader-variant count is recorded and bounded.
- [ ] Pipeline-key count and cache-hit behaviour are measured.
- [ ] Unsupported combinations have deterministic material fallbacks.
- [ ] Runtime pipeline-compilation stalls are measured and reduced.
- [ ] Feature combinations do not create uncontrolled pipeline explosion.

## MAT-010 — Normal-map convention — REQUIRED to accept both

Arma commonly uses DirectX-style tangent-space normals: `+X, -Y`. glTF and many OpenGL workflows use `+X, +Y`.

**Suggestion:** use `+X, -Y` as the canonical Poseidon cache convention because Arma compatibility is a core goal, but agents may recommend another convention after evaluating the whole pipeline.

- [ ] Convention is explicit metadata.
- [ ] Arma `_NO`/`_NOHQ` defaults to negative Y.
- [ ] glTF defaults to positive Y.
- [ ] Unknown maps require declaration or visual validation.
- [ ] Offline conversion flips the green channel when needed.
- [ ] Original source is preserved.
- [ ] Validation scene detects inverted normals.

## PAK-000 — Immutable overlay packages — REQUIRED

Suggested mount order:

```text
User/mission override
Poseidon HD package
Compatibility package
Original game data
Emergency fallback
```

- [ ] Original files remain untouched.
- [ ] Packages can be removed safely.
- [ ] Canonical path plus known source hash prevents replacing unrelated mod content.
- [ ] `--safe-assets` disables optional replacements.

## PAK-010 — Per-channel fallback — REQUIRED

```text
Albedo: HD → original → emergency checkerboard
Normal: HD → generated/detail → flat normal
Roughness: HD → material default → safe constant
AO: HD → 1
Metallic: HD → material default → 0
Height: HD → 0
Opacity: HD → original alpha → opaque
```

## PAK-020 — Package validator — REQUIRED

- [ ] Manifest and file validation.
- [ ] Hashes, dimensions, mips, compression, colour space.
- [ ] Normal-map convention and physical scale.
- [ ] VRAM estimate and fallback availability.
- [ ] Original source URL and licence record.
- [ ] Derived-asset provenance.
- [ ] Gameplay-critical versus visual-only classification.


## STR-000 — World streaming and residency strategy — INVESTIGATE before large-scale content expansion

Large worlds, HD packages, retained GPU scenes, terrain chunks, collision data, water domains, probes, far-world representations, and secondary views compete for finite storage, RAM, staging bandwidth, and VRAM. The required decision is how Poseidon should budget, prioritise, load, retain, and evict these resources; one global scheduler is a candidate, not a predetermined conclusion.

The investigation should compare:

- Existing engine streaming and cache behaviour.
- Specialised per-subsystem managers.
- A shared residency budget with subsystem-owned policies.
- Predictive prefetch based on player movement, vehicles, aircraft, Zeus cameras, missions, and cinematics.
- CPU decoding/transcoding, asynchronous I/O, staging, and GPU upload strategies.
- Cold-start, warm-cache, teleport, fast-flight, and memory-pressure behaviour.

Required outcomes:

- [ ] Named RAM, VRAM, staging, I/O, and derived-cache budgets per platform tier.
- [ ] Clear ownership of residency decisions and resource lifetime.
- [ ] Graceful degradation under pressure rather than crashes or permanent stalls.
- [ ] Debug views for residency, eviction, pending uploads, cache misses, and pop-in causes.
- [ ] Metrics for upload stalls, visible pop-in, cache churn, and secondary-view amplification.
- [ ] Compatibility with immutable originals, optional packages, and safe fallbacks.
- [ ] A decision to adopt a shared scheduler, adapt existing systems, retain specialised managers, or defer broader unification.

## STR-010 — Implement approved streaming and residency improvements — IMPLEMENT only after decision

Depends on an `ADOPTED` or `ADAPTED` outcome from `STR-000`.

- [ ] Implement only the selected scope.
- [ ] Introduce budgets and instrumentation before aggressive eviction or prefetch logic.
- [ ] Preserve subsystem fallbacks and avoid a flag-day migration.
- [ ] Validate stationary, high-speed vehicle, aircraft, Zeus-teleport, and memory-pressure scenarios.
- [ ] Do not make distant content authoritative merely because it is streamed for rendering.

---

# 8. Grass texture findings and vegetation roadmap

## Core finding — REQUIRED direction

The existing procedural grass geometry should be able to use authored PBR texture data. Procedural systems remain responsible for placement, wind, helicopter downwash, tracks, flattening, explosions, and LOD. Textures can provide albedo, opacity, normals, roughness, AO, thickness/transmission, seed heads, dryness, and species variety.

## Suggested discovery links — suggestions only

- Public-domain grass search:
  https://3dassets.one/?q=grass&license=public-domain&sort=popular
- Public-domain grass models:
  https://3dassets.one/?q=grass&license=public-domain&type%5B%5D=3d-model&sort=popular
- Poly Haven: https://polyhaven.com/textures
- Poly Haven licence: https://polyhaven.com/license
- ambientCG: https://ambientcg.com/
- ambientCG licence: https://docs.ambientcg.com/license/

Examples worth evaluating, not mandatory selections:

- https://polyhaven.com/a/grass_medium_01
- https://ambientcg.com/a/Grass001
- https://ambientcg.com/a/Grass004

Agents should compare these with other CC0/licence-compatible or original assets. Aggregator licence labels are not authoritative; verify the original provider page.

## ASSET-010 — Third-party source registry — REQUIRED

For every selected asset record source URL, provider, author, licence, licence URL, download date, archive hash, processing version, derived hash, and intended use.

- [x] CI rejects unregistered derived assets — `scripts/validate_asset_registry.py` runs in the
      Preview-0 lint job. Verified it can fail: a tampered source byte, a second SELECTED entry and
      a removed `licence_url` are each rejected.
- [x] Source-to-derived provenance is complete — `docs/assets/source-registry.yaml` records source
      URL, provider, author, licence, licence quote, download date and per-file hashes, plus the
      derived blade layers with their recipe version and hashes.
- [x] Proprietary marketplace or game assets are not committed unlawfully — the registered source
      is CC0 from its original provider. The pre-existing `assets/grass` PNGs are recorded as LOCAL
      SCRATCH and deliberately NOT registered: their origin is recalled but unverifiable, and a
      guessed URL would look checked without being checked.

## GRS-GATE-1 — One complete grass-asset path — REQUIRED before a large species library

Prove one complete, reversible path first:

```text
One licence-compatible source candidate
→ verified original-provider licence
→ source registry entry and hashes
→ deterministic processing recipe
→ validated optional package
→ texture array
→ near/mid/far rendering
→ procedural fallback when absent
→ visual and performance comparison
```

- [x] Exactly one initial source path is selected after comparing candidates — PBRPX
      `PX_Echinochloa_crus_galli_Leaf_01` (CC0, verified on the provider's own site, not on the
      aggregator that surfaced it). Chosen after comparing opacity masks against
      `PX_Imperata_cylindrica_Atlas_09`, which is seed-head stalks rather than leaf blades; the
      rejected candidate stays registered so the comparison is evidenced rather than asserted.
- [x] Original game data remains untouched — the authored path reads loose files beside the
      binaries, never a PBO.
- [x] Removing the package restores procedural/original fallback — deleting the blade layers falls
      the near ring back to the generated atlas, all-or-nothing by design.
- [x] Missing or corrupt textures cannot prevent startup — absence and mismatch both log at INFO
      and fall back.
- [x] Build outputs are reproducible — `scripts/build_grass_blade_atlas.py` rebuilds the eight
      layers deterministically from the registered source; `--check` is a real determinism test.
- [~] VRAM, overdraw, and GPU cost are recorded — cost and VRAM are (2.179 ms of a 24.178 ms frame;
      512 KB, unchanged between procedural and photo since the array dimensions are identical).
      OVERDRAW is not: there is no overdraw capture yet, and the alpha-card path is exactly where
      one would matter (+0.390 ms measured on the colour pass when enabled).
- [ ] Only after this gate passes may agents expand to a broad species library.

## GRS-010 — PBR grass texture arrays — SUGGESTED

Candidate layers:

```text
Albedo
Opacity
Normal
Roughness
AO
Thickness/transmission
Optional height
Species mask
Dryness mask
```

- [ ] Candidate assets compared for near blades, seed heads, clumps, and far ground.
- [ ] Art direction and performance reviewed.
- [ ] Licensing complete.

## GRS-020 — Offline grass-processing pipeline — SUGGESTED

Compare high-poly baking, individual-blade extraction, retopology, directional clump atlases, scanned blade cards, procedural generation, and hybrids.

Outputs should include alpha dilation, alpha-coverage-preserving mips, correct colour-space and normal metadata, deterministic processing, and source hashes.

Required comparison and acceptance tests:

- [ ] Alpha test versus alpha-to-coverage where supported.
- [ ] Coverage-preserving mip reference images.
- [ ] Grazing-angle and anisotropic filtering tests.
- [ ] Thin-blade normal reconstruction tests.
- [ ] Overdraw heat map.
- [ ] Shadow-map alpha stability.
- [ ] Temporal stability under dithered LOD and any temporal reconstruction in use.
- [ ] Texture-array residency and streaming budget.

## GRS-030 — Species and LOD

- [ ] Species stiffness, damping, flutter, transmission, dryness, height, and biome rules.
- [ ] Detailed near blades.
- [ ] Simplified near-mid clumps.
- [ ] Mid-distance cards/clumps.
- [ ] Far terrain vegetation coverage, macro normals, roughness, gust shading, and sparse silhouettes.
- [ ] Ground, vehicle, helicopter, and aircraft tests.
- [ ] Stable alpha and dither transitions.

### GRS-030 direction — the rendering UNIT is a clump, not a blade (added 2026-08-06)

The items above can all be satisfied with better crossed cards, and that is exactly what was tried
and rejected. Recording the direction so the next attempt does not re-run the same experiment.

**What was tried, 2026-08-05.** Eight distinct species silhouettes, per-blade taper and bend jitter,
a photographed blade atlas, a retuned taper (the exponents were making spears, not blades), and
height-proportional arch. Each was a real improvement and the owner's verdict after all of them was
still "grass silhouette never worked". The conclusion is not that the textures were wrong. It is
that ONE INSTANCE IS ONE PLANT — two crossed ribbons of five segments — and no amount of shaping a
single crossed plant produces the look of a meadow.

**The direction.** For the first hero species, near and near-mid instances should each be a
multi-blade three-dimensional CLUMP: several curved blade ribbons with their own heights, widths,
bends and orientations, in several distinct clump silhouettes. Geometry carries the near silhouette;
opacity cutouts are for fine detail and for more distant representations, not for the near
silhouette.

**Why geometry rather than cutouts, measured here rather than argued from principle:**

- Enabling the alpha cut-out card path cost **+0.390 ms on the grass colour pass, +37%**.
- Worse, putting that cutout behind a runtime flag INSIDE the shader cost **+67% with the feature
  switched OFF**, because the mere presence of `discard` disables early-Z for the pipeline. It had
  to be split into its own entry point and pipelines.
- The shape work that made blades read correctly — eight profiles, jitter, arch — cost **+0.09 ms
  total**, effectively nothing, because it is vertex-shader width and curve.

So on this renderer, geometry is the cheap axis and transparency is the expensive one. That is the
opposite of the usual instinct and it should not have to be rediscovered.

**Budget the change against what is there now.** The near ring currently places 48 647 instances at
60 vertices each, about 2.9 M vertices, for 2.18 ms of a 24.2 ms frame. A clump of eight blades
replaces roughly eight instances, so instance count should FALL while vertex count stays in the same
range. Placement grids (`GRID_DIM` 512, mid/far 384) and the shared species ABI with `grass.wgsl`
are what the change actually touches.

- [ ] Several distinct clump silhouettes per hero species; no obviously repeated silhouette in a
      single view. Repeated-silhouette inspection is required evidence, not a matter of taste.
- [ ] Species identity, proportions and colour preserved across near, mid and far, so a plant does
      not change character as the player walks toward it.
- [ ] Near and mid must not depend on camera-facing crossed cards.
- [ ] Mid replaces the single photographed crossed tuft first — it is the weakest of the three and
      the cheapest to prove.
- [ ] Lighting: normals reconstructed AFTER wind deformation, two-sided response, species-authored
      thickness and transmission colour rather than one shared backlighting term, root-to-tip
      variation, terrain-colour influence at the base, and GTAO restrained on thin blades so they do
      not read as dirty black lines.
- [ ] Placement at three scales — biome, metre-scale patch with deliberate bare gaps, and individual
      plant — driven by terrain material, slope and moisture. Uniform coverage is one of the fastest
      ways to make procedural grass look artificial.
- [ ] Motion separates coherent whole-clump sway from independent blade-tip flutter, with authored
      per-species resting bend, stiffness, damping, gust response, wind straightening and recovery.
- [ ] Existing tracks, flattening, explosions, helicopter downwash and the procedural fallback keep
      working throughout. These already distinguish this grass and must not be traded away.
- [ ] Evidence: matched ground/vehicle/helicopter/aircraft captures, overdraw heat map, triangle and
      instance counts, grass GPU time, shadow stability, and slow-camera-rotation shimmer.

**One correction to note.** An external review of this system asserted that the coverage-preserving
mip implementation "is sound and should be retained". There is no such implementation for grass —
alpha-to-coverage exists only on the foliage path for trees and objects, and coverage-preserving
mips remain an unchecked `GRS-020` item. Do not plan around retaining something that has not been
built.

## GRS-040 — Preserve helicopter downwash — REQUIRED

Preserve current rotor-speed, altitude, inertia, multi-helicopter, visible deformation, and matching shadow behaviour.

Suggested improvements:

- Rotor centre, axis, disc radius, and aircraft attitude.
- Ground-impact projection and radial wall jet.
- Turbulent outer ring.
- Terrain slope and building obstruction.
- Shared dust, smoke, spray, and snow response.

---

# 9. Weather, wind, clouds, and lighting

## ATM-000 — Authoritative WeatherState — REQUIRED

Suggested fields: seed, overcast, rain, snow, fog, humidity, temperature, surface wind, gusts, turbulence, storm intensity, and transition ticks.

- [ ] Server owns targets.
- [ ] Clients reconstruct detailed visuals.
- [ ] Late join and save/load work.
- [ ] Zeus edits weather through server-authoritative commands.

## ATM-GATE-1 — Weather and atmosphere consumer migration — REQUIRED before field expansion

`WORLD-003` introduces the interface. This gate proves current consumers use it.

- [ ] Existing grass wind and gust sampling migrated without visual regression.
- [ ] Current weather/time readers migrated or explicitly documented.
- [ ] Water-surface wind input migrated.
- [ ] Cloud input path identified.
- [ ] Smoke and ballistics integration points identified.
- [ ] Bypasses are removed or recorded as blocked tickets.
- [ ] Two-client authoritative transition test passes.

## ATM-010 — Shared atmosphere/wind field — REQUIRED outcome, implementation INVESTIGATE

Consumers: grass, smoke, dust, clouds, water surface detail, ballistics, aircraft, fire, snow, and possibly audio.

Suggested migration: preserve the current grass gust equations first, centralise them, then make other systems sample the same deterministic field.

## ATM-020 — Representative local airflow integration

Preview 1B requires at least one representative integration—preferably preserved and improved helicopter downwash—through the shared atmosphere interface.

- [ ] One authoritative or deterministically reconstructed emitter input.
- [ ] Grass response.
- [ ] Smoke response.
- [ ] Debug visualisation.
- [ ] Multiplayer and late-join behaviour.
- [ ] Broader prop wash, jet exhaust, vehicle wake, explosions, fire, and ventilation remain suggested later coverage.

## ATM-030 — Obstacle-aware local wind — INVESTIGATE

Compare local velocity grids, occupancy or SDF projection, a small pressure solve, precomputed building hints, and analytic deflection. Do not attempt full-world CFD.

## CLD-010 — Weather-connected volumetric clouds — REQUIRED

Cloud coverage, density, type, altitude, vertical growth, wind, precipitation, and extinction derive from weather. Clear, scattered, broken, overcast, storm, and foggy presets are suggestions, not immutable mappings.

## CLD-020 — Cloud-shadow response — REQUIRED outcome for Preview 1A — **IMPLEMENTED 2026-08-05**

Preview 1A requires a functioning cloud-shadow solution. The suggested implementation is near/far sun-transmittance clipmaps with temporal reprojection, wind-compensated history, partial updates, and quality scaling.

Shipped as the **approved cheaper fallback**, not the suggested clipmaps: one compute pass marches
the same `cloud_density` field the sky raymarch uses, from each ground texel toward the sun, into a
world-anchored 512x512 map covering 4 km around the camera. Every lit surface then costs one
texture tap. No temporal history, no reprojection, no partial updates — and therefore none of the
failure modes those bring. `wgr_set_cloud_shadow_strength`; slider in the Sky tab.

An approved cheaper fallback is acceptable when it:

- [x] Responds to authoritative cloud/weather state — it marches the authored cloud field itself,
      so coverage, extinction, deck altitude and wind drift all feed it with nothing to keep in sync.
- [x] Affects terrain and major opaque geometry consistently — terrain, objects, grass and water all
      sample the same map, so a cloud dims the whole scene together rather than only the ground.
- [x] Has a clearly documented quality limitation — three, in the code and the tooltip: the map is
      evaluated at SEA LEVEL, so a hillside and the valley beneath it get the same shadow; it covers
      a finite 4 km square, outside which surfaces read fully lit (never dark — missing data must
      not invent shadow); and its origin is snapped to its own texel grid, without which the pattern
      crawls with camera movement.
- [x] Fits the declared platform budget — below run-to-run noise at the Tier 1 configuration
      (26.216 -> 26.145 ms frame total, i.e. unmeasurable against variance).
- [x] Can later be replaced without changing gameplay state — it is a render-only transmittance
      map read by shading; nothing in simulation, scripting or multiplayer observes it.

Verified on 2026-08-05: zero validation errors, clear sky unchanged, forced overcast removes direct
sun scene-wide including a soldier's own cast shadow, partial coverage puts the camera in a lit gap
with that shadow back. **Not** verified: a shadow edge sweeping the ground in motion, which needs a
person watching it.

## CLD-030 — God rays — REQUIRED outcome

Prefer volumetric atmosphere/fog sampling of cloud sun transmittance rather than a purely fake radial post-process. Test clear, broken, full-overcast, storm, and fog scenes, with solid-geometry occlusion and stable temporal behaviour.

## LIT-010 — GTAO — SUGGESTED — **IMPLEMENTED 2026-08-05**

Use for contact grounding and corners, not as the sole solution for dark interiors.

Shipped: scalar AO (Stage 1), bent-normal directional ambient (Stage 2), and a hierarchical
mip march (Stage 3, **default off**). See `engine/WgpuRenderer/docs/screen-space-ao-plan.md`.
Default ON, own dev-tools tab, three-way debug view (off / AO / bent normal).

- [x] Contact grounding and corners — owner-confirmed, "i think i prefer it on".
- [x] Ambient term only; direct sun and local lights untouched.
- [x] **Frame cost measured (2026-08-05)** and the open box closed. Three GPU-timer regions
      (`GtaoPrep` / `GtaoCompute` / `GtaoBlur`), rows in the Amb. Occlusion tab, and a periodic
      `Lighting GPU ms:` log line so a run can be diffed against a previous build rather than
      compared by screenshot. Measured at 800x600 windowed, RWDI:
      `ao_prep=0.096 ao_march=0.273 ao_blur=0.148 ao_total=0.518 | frame=6.857` — **~8% of the
      GPU frame**, with the horizon march half of it (so slices x steps is the knob if it ever
      needs to come down).
- [x] **Default ON** as of 2026-08-05, on the owner's instruction, with the cost above measured in
      the same change.
- [ ] **The 8% figure does not survive contact with the Tier 1 configuration.** Re-measured the
      same day in the original training mission at 3441x1440, RWDI, Vulkan, RTX 3070:
      `ao_prep=0.299 ao_march=6.335 ao_blur=1.089 ao_total=7.723 | frame=24.178` — **31.9% of the
      GPU frame**, and the horizon march alone is the largest single item in that frame by more
      than five times the next one. Evidence:
      `docs/roadmap/evidence/preview0-wgpu-rel000-mission-20260805-original-training-capture.json`.

      Resolution accounts for most of the gap and not all of it: the pixel area ratio between
      800x600 and 3441x1440 is 10.3x, while the march grew 23x. So this is not the same
      measurement scaled up, and **the 8% number should not be quoted for any real
      configuration** — it describes an 800x600 window and nothing else. Recorded here rather
      than quietly replaced, because that figure is what "measured, ~8%" was resting on.

      What this needs is a quality-versus-cost decision, not another measurement. `slices x
      steps` is still the knob, and it now has a reason to be turned.

      **Lead, from reading a reference project (2026-08-06).** Esoterica runs Intel's XeGTAO at
      HALF RESOLUTION with a bilateral upsample, over a 4x-downsampled depth
      (`Code/Engine/Render/RenderPasses/RenderPass_GTAO.cpp`). Our 6.335 ms is the horizon march,
      which is per-pixel, so quartering the marched pixels is the largest single lever available and
      is a bigger win than trimming `slices x steps`. Ours already has the depth-mip prefilter half
      of that structure; what is missing is running the march at half res and upsampling.

      XeGTAO also carries `ThinOccluderCompensation`, which is aimed at exactly the failure this
      roadmap already warns about for vegetation -- thin blades reading as dirty dark lines.

      Licence note before anyone copies: XeGTAO is Intel's, MIT, VENDORED INSIDE Esoterica. It is
      not covered by Esoterica's own MIT grant and is the precise case `CORE-007` exists for. Take
      it from Intel's own repository with its notice, or reimplement the technique.

The line above about dark interiors held exactly as written: GTAO's radius is ~2 m and a room is
larger, so interiors barely changed. That is `LIT-020`, below.

## LIT-020 — Geometry-aware interior sky visibility — REQUIRED outcome — **BOTH STAGES IMPLEMENTED AND DEFAULT ON 2026-08-05; NOT RE-JUDGED, AND STAGE 2 IS UNHARDENED**

Investigate building voxelisation, outside flood fill, sky-visibility probes/volumes, and door/window portals.

- [x] Investigated; design recorded in `engine/WgpuRenderer/docs/interior-sky-visibility-plan.md`.
      Chosen approach: directional depth maps from a FIXED set of sky directions (zenith + tilted),
      taking the unoccluded fraction. Voxelisation and portals are kept as Stage 2 fallbacks.
      A bounding-sphere roof map was evaluated and rejected — the renderer has no boxes, and a
      sphere around a building darkens the street outside it.
- [x] Implemented: five-direction depth maps (zenith + 4 tilted ~50 deg) at 6 cm/texel, one cull
      view and one depth-array layer each, drawn by the existing GPU-driven shadow depth pipeline;
      kernel-softened, slope-scale-biased, with the sky irradiance STEERED toward the reach-weighted
      open direction rather than merely scaled by it. Debug reach view, GPU-timer rows, dev-tools
      tab, `Ctrl+Shift+I` / `Ctrl+Shift+O` hotkeys, default OFF.
- [x] Hardware ray tracing evaluated and rejected — **but the stated reason was wrong, corrected
      2026-08-05.** wgpu 29 does gate ray queries behind an experimental Vulkan-only feature. The
      rest of the sentence, "while this renderer runs DX12 on Windows", is false: nothing in the
      tree pins a backend, and every recorded run on the Tier 1 machine — runtime logs, Preview-0
      manifest, and the Tier 1 validation contract alike — reports a **Vulkan** adapter. The
      rejection survives on the argument that was never made: backend selection is not pinned, so
      a DX12 or Metal user gets a different answer, and a lighting term cannot exist only on some
      backends while GL33 remains a supported fallback. The raster path had to exist either way.
- [x] **Stage 2 built the same day the parking notice was written** (`e74041f`..`a3e80aa`): the
      per-model sky-visibility bake, fed by real LOD-0 geometry straight out of the shared geometry
      pool, sampled per fragment, storing a DIRECTION per voxel alongside the scalar. Both stages
      are now **default ON**.
- [ ] **Not re-judged.** The owner verdict below was recorded against the per-frame-only version.
      Nobody has looked at the baked path in a real building, so the acceptance list is stale in
      the optimistic direction, not the pessimistic one.
- [ ] **Stage 2 is unhardened.** §3d's requirements do not exist in the code: no content-hashed
      disk cache, no background scheduling with a reach = 1 fallback, no model-variance policy.
      `sky_bake.rs` is synchronous by design and the layer that was supposed to sit above it was
      never written.
- [x] **The load-stall estimate was wrong, and measuring it corrected it downward.** Earlier
      today this said "~500 models is a ~10 s main-thread load stall, shipping by default" and
      called it a release risk. Measured 2026-08-05 in the original training mission: **8 models,
      ~0.29 s total** (`preview0-wgpu-rel000-mission-20260805-original-training-capture.log`).

      The per-model arithmetic was never the problem. The bake runs once per **registered** model
      — the models a mission actually loads — and the "~500" was the size of the model library,
      which nobody checked against what a mission registers. Per-frame cost is likewise small and
      now measured: `Interior sky: cull 0.408 + depth maps 0.163 = 0.570 ms`, 2.4% of the frame.

      The cache and background scheduling are still required: a large mission on a full island
      registers far more than eight, and that number is still unmeasured. But this is no longer a
      reason to hold a release, and the `REL-000` caveat is corrected accordingly. Worth noting
      which way the error ran — the estimate was pessimistic, and it was repeated three times
      before anyone ran the mission.

**Owner verdict (2026-08-05), against the per-frame-only version.** Foliage improved — "trees look better with interior ambient
enabled". Interiors did not: *"the patches of strange shadows is still a problem inside
buildings"*. Two rounds of fixes (nine-tap two-ring kernel for the banding, slope-scaled bias for
grazing-angle self-shadowing) reduced it but did not remove it. Parked on the owner's instruction:
*"i also do not want to continue on this for now unless you know how to fix it for good"*.

**The diagnosis recorded here on 2026-08-05 was wrong, and the correction matters.** This section
argued the patches were structural to sampling a CAMERA-space depth map — a texel grid unrelated to
the surfaces receiving the result, so the lit/occluded boundary lands on the grid instead of on the
wall. That reasoning was sound and the conclusion was still false. The tester isolated the real
cause by setting `directional = 0`, which removes the patches entirely: it was the **directional
steering**, not the depth sampling. `interior_sky_ambient_normal` bent the shading normal toward
the reach-weighted open direction across only FIVE sampled directions, so the steered normal jumped
between discrete values and the sky-irradiance lookup jumped with it. Hard edges that look like
geometry, produced by quantisation. Recorded rather than quietly amended, because "structural to
the approach" is exactly the kind of claim that stops the right fix being attempted.

**The per-model bake (§3c / option E) was then built anyway, and it is the right home for the
steering.** It resolves occlusion in MODEL space, trilinearly filtered, attached to the building's
own geometry with no camera-relative grid to alias against, and it stores the average incoming sky
DIRECTION per voxel integrated over 41 directions — so steering is smooth, with nothing left to
quantise. Falsifier built into its test suite: the window-side-brighter assertion is paired with
the SAME room with the opening filled, which must NOT show the gradient. Both pass.

**What is missing is §3d, and it is missing entirely.** Content-hashed disk cache, background
scheduling with a reach = 1 fallback, and the model-variance policy (animated doors, damaged and
destroyed models, alpha-tested glass, proxies, visual LODs, and mirrored or negatively scaled
instances — a model-space volume is orientation-free but is NOT mirror-free). `sky_bake.rs` is
deliberately pure and synchronous; the layer that was to sit above it does not exist. At ~18-22 ms
per model that is a ~10 s load stall on a full library, currently on by default.

Acceptance (recorded against the per-frame-only version; **not yet re-judged** against the baked
path, which is what the remaining item below is actually asking about):

- [x] Porch partly dark.
- [~] Window-adjacent room receives light — the tilted maps do let light in, but the surrounding
      surfaces carry the patch artifact, so this is not owner-accepted.
- [x] Deep room is dark.
- [x] Sealed bunker has no unexplained ambient skylight.
- [x] Local lights continue working.

## LIT-030 — Relightable indirect and probe lighting — INVESTIGATE

After geometry-aware sky visibility is understood, investigate whether relightable probes, precomputed transfer data, light volumes, selective dynamic injection, screen-space techniques, ray queries, or a hybrid can improve indirect lighting and indoor/outdoor transitions.

Required questions:

- Can static original geometry receive convincing changing sky and sun illumination without modifying original source assets?
- Can dynamic objects sample stable indirect light near windows, doors, porches, forests, and interiors?
- What is the fallback when derived lighting data is missing or invalid?
- How are terrain deformation, destruction, weather, time of day, and optional HD assets handled?

- [ ] Compare quality, build time, storage, runtime cost, update cost, portability, and failure behaviour.
- [ ] Do not assume one global-illumination technique is appropriate for every platform tier.
- [ ] Preserve local lights and the legacy ambient fallback.
- [ ] The initial investigation does not block Preview 1A.
- [ ] A superior specialised interior or exterior solution may replace a universal probe design.

## LIT-EXP — Optional ray queries — EXPERIMENTAL

May refine interior visibility, contacts, or reflections. Must be optional, feature-detected, non-authoritative, and have a raster fallback.

---

# 10. Unified water

## Principle

All water belongs to one engine-facing `WaterWorld`, while internal techniques may include spectral waves, shallow-water domains, breakers, local interactions, particles, and baked states. Public products such as Fluid Flux and Easy Waterscape may be visual/workflow references only; implementation must be clean and legally independent.

## Unified-water prerequisites

- See the canonical `WTR-GATE-1` definition in Section 6.
- Complete `WORLD-002`.
- Complete `WTR-GATE-2` before adding broad new backends.

## WTR-GATE-2 — WaterQuery consumer migration complete — REQUIRED

- [ ] Boat and buoyancy consumers use `WaterQuery`.
- [ ] Character/submersion consumers use `WaterQuery`.
- [ ] Projectile water-crossing logic uses `WaterQuery`.
- [ ] AI and audio consumers are migrated or explicitly N/A.
- [ ] Rendering consumers needing canonical body data are identified.
- [ ] No unexplained gameplay-critical direct readers remain.
- [ ] Baseline behaviour matches the pre-migration implementation.

## WTR-100 — WaterBody registry and backend expansion — REQUIRED

Begin only after `WORLD-002`, `WTR-GATE-1`, and `WTR-GATE-2`.

Body types may include ocean, lake, river, pond, flood, and waterfall. Backends may include static, analytical, spectral, shallow-water, and hybrid.

- [ ] Canonical body identity and ownership.
- [ ] Explicit body type and backend.
- [ ] Streaming and lifecycle rules.
- [ ] One small non-ocean body proves the registry before broad expansion.
- [ ] Existing ocean remains a valid backend rather than being rewritten merely to fit an abstraction.

## WTR-110 — Generalise current FFT — SUGGESTED

- [ ] JONSWAP or another verified spectrum model.
- [ ] Separate wind sea and swell.
- [ ] Independent cascades.
- [ ] Weather transitions.
- [ ] CPU-compatible low-frequency queries.
- [ ] Orbital velocity, slope variance, and depth filtering.

## WTR-120 — Anti-repetition — INVESTIGATE

Compare non-harmonic lengths, independent seeds/directions, spread variation, long swell, world-scale distortion, and alternative spectral layouts. Require aerial and pitch-stability tests.

## WTR-130 — Persistent foam — SUGGESTED

Combine spectral compression, shallow-water turbulence, breakers, shoreline run-up, wakes, projectiles, explosions, and waterfalls. Advect, accumulate, and decay with stable history.

## WTR-140 — Reflection ownership — REQUIRED

Environment owns sky/clouds, SSR owns confident nearby opaque geometry, and planar reflection owns explicitly selected content. Add contribution and confidence debug views.

## WTR-200 — Shallow-water prototype — INVESTIGATE

Prototype depth, velocity, foam/turbulence, bathymetry, obstacles, sources, drains, wet/dry transitions, friction, rain, and impulses. Compare numerical methods, 16/32-bit storage, a CPU reference, and GPU cost.

Quantitative acceptance criteria must include:

- [ ] Mass-conservation error over a defined test duration.
- [ ] Maximum stable timestep.
- [ ] Wet/dry oscillation behaviour.
- [ ] Boundary-reflection behaviour.
- [ ] Momentum introduced by test impulses.
- [ ] Domain-handoff error.
- [ ] Sleep/wake hysteresis.
- [ ] Maximum active-domain budget.
- [ ] Recovery after a snapshot/correction.
- [ ] Server coarse-state versus client visual-state agreement.

## WTR-210 — Nested domains, baking, sleeping — SUGGESTED

Coarse world, regional, and high-resolution interaction domains exchange boundaries. Cap active domains through an explicit scheduler.

## WTR-220 — Expanded river and coast integration — CONDITIONAL later work

This ticket is not required for the narrow Milestone 5 WaterQuery interoperability proof unless an approved release slice explicitly depends on it.

Potential scope:

- [ ] River spline geometry: width, height, depth, flow, friction, and banks.
- [ ] Tributary and source/drain connections.
- [ ] Coast inputs: shore distance, bathymetry, slope, waves, current, and material.
- [ ] Basic rapids representation.
- [ ] Basic waterfall representation and receiving-water coupling.
- [ ] Functional multiplayer validation for the activated scope.
- [ ] Advanced cinematic breaker and waterfall behaviour is explicitly deferred to `WTR-221`.

A stronger agent may replace the spline or coupling mechanism when another representation better serves imported worlds, authoring, streaming, multiplayer, or terrain interaction.

## WTR-221 — Advanced breakers and waterfalls — OPTIONAL later expansion

Potential later work:

- [ ] Detailed plunging and spilling breakers.
- [ ] Sheet breakup and aeration.
- [ ] Spray interaction with wind and geometry.
- [ ] Multi-stage waterfall aeration.
- [ ] Advanced receiving-pool turbulence.
- [ ] High-quality foam and particle coupling.

## WTR-190 — Water optical model and underwater transition — NON_BLOCKING_VALIDATION, INVESTIGATE

Origin: external proposal, reviewed in [`WTR-external-proposal-review-20260802.md`](../decisions/WTR-external-proposal-review-20260802.md). Renumbered from the proposal's `WTR-150`, which names *Optional local volumetric fluid rendering* in the Water Master Plan. **Scope this as a revision**, not greenfield: `WTR-050`/`WTR-100`–`WTR-120` in the Master Plan cover the same ground, and absorption/scattering terms already ship in `water/water.wgsl`.

**Trimmed to the novel part.** The proposal's full optical model (absorption, scattering, depth colour, turbidity) largely restates Master Plan `WTR-050`/`WTR-110` and code that already ships. What is genuinely unspecified anywhere is the **surface-crossing transition**: the engine has no defined behaviour for the frames in which the camera passes through the waterline. Restrict this ticket to that, and fold any remaining optical requirement into the Master Plan phases.

- [ ] Exposure, fog and colour are continuous across the crossing. Verifiable form: sample mean luminance over a fixed centre region across a scripted descent, and require frame-to-frame change no worse than the largest change seen in the ten frames before entry.
- [ ] The transition is symmetric — entering and exiting produce the same sequence reversed, within the same tolerance.
- [ ] Optical effects stay cosmetic: `WaterQuery` height and depth results are bit-identical with the effect forced on and off.
- [ ] Missing optical data falls back to the current path rather than failing the frame.
- [ ] Captures exist above water, at the waterline, and below, from one scripted camera path.
- [ ] GPU cost measured on the `PERF-001` Tier 1 configuration, since that is the only tier this project has defined.

Volumetric caustics, ray tracing and any one scattering model are explicitly *not* required.

**Landed so far** (none of the boxes above are met yet; this is groundwork):

- `2d06adc` — `fft_control.w` carries eye submersion in metres rather than a 0/1 flag, and
  the compositor ramps by it instead of snapping to a fixed `-0.08`. This removed the pop as
  a crest passed the eye, which was the most visible discontinuity across the crossing.
- `6d73940` — the compositor locates the surface by marching each ray against the FFT
  displacement rather than intersecting a flat plane at `sea_level`. A crest overhead now
  counts as submersion on every cascade preset, not only the one whose surface the CPU can
  reproduce; and a straddling view resolves into a real waterline instead of one whole-screen
  answer. Behaviour is pinned by a Rust mirror of the march over an analytic swell, including
  a calm-sea case that must still reproduce the plane exactly.

- `d76b787` — the compositor no longer contributes anything from above the surface. Water
  renders with `depth_write_enabled: Some(false)`, so the depth target this pass samples never
  contains the water surface; the flat model treated the plane crossing as an entry point and
  fogged the whole scene behind it, which is why the effect appeared to engage while the camera
  was clearly in open air. Those pixels belong to the water shader. Absorption hue is also now
  derived from the authored deep swatch rather than a hardcoded curve, so the volume is the same
  substance as the surface.

**Parked, effect off by default** (`3b60350`). The owner's judgement is that the look is not
good enough to ship on, so `WaterSettings::underwaterEffect` now defaults to false and the
effect is opt-in from the dev overlay's Water tab. This does not retire the ticket — the
surface-crossing contract is still undefined and still wanted — but nothing here is on the
critical path while the effect is off, and it should not be treated as blocking.

Still open before any box can be ticked: the scripted camera path and its three captures
(above, at the waterline, below), the luminance-continuity and symmetry measurements, the
`WaterQuery` bit-identity check, and the `PERF-001` GPU cost. The camera path is the
blocker, but it is no longer unexplained.

The "broken pipe after about 59 s" is diagnosed and reproduced — see
`tests/integration/water/water_underwater_probe.test.sqf`. The camera going underwater is
not the problem: that probe walks the camera from 6 m over the datum to 6 m under and back
and the game stays healthy. The trigger was `triCamPos`, which is viewer-mode only and
returns `FAIL:not_viewer_mode` under `PoseidonGame`. Trident retries a `FAIL:` statement
until `assert_timeout` while the game-side harness answers "command timed out" after 30 s,
so a merely-wrong statement surfaces at ~60 s as a transport error naming neither the real
result nor the log that has it. A plain SQF syntax error presents the same way, because a
script error aborts the process in test mode.

Practical rule for anything driving this harness: "broken pipe" means read the game log.

What still blocks the captures is narrower than it looked: there is no harness command that
reports submersion, so a camera sweep cannot assert it actually went under — `triWaterStats`
reports live water whatever the camera does. A `triWaterSubmersion` alongside the existing
water observability commands is the missing piece. Note also that any capture run must now
enable the effect explicitly, since the default is off.

## WTR-250 — Precipitation: occluded rain, wet surfaces — REQUESTED, NOT STARTED

Owner request, 2026-08-03, recorded rather than started. The Zeus weather controls
landed the same day expose overcast and fog and drive `World::SetWeather`; **rain is not
wired at all**, so a storm preset gives cloud and gloom with a dry sky.

Wanted, in the owner's terms:

- A real **particle rain system**, not a screen overlay.
- Rain that **does not fall under bridges, buildings or other cover** — i.e. occlusion,
  not a uniform camera-locked effect.
- Streets and surfaces that **go visibly wet** while it rains, and presumably dry after.

Notes for whoever picks this up:

- `Landscape::SetRain(density, time)` already exists and `GetRainDensity()` reads back, so
  the *state* plumbing is present; what is missing is the rendering and the occlusion.
- The occlusion requirement is the hard part and the reason this is its own ticket rather
  than a Zeus-tab checkbox. The obvious cheap source is the terrain shadow map / sky
  visibility already used for ambient (`RSYS-SKY-VIS-AO`), which answers "can this point
  see the sky" — the same question rain occlusion asks. Reusing it would be squarely in
  the spirit of `RND-030`'s reuse box; building a second occlusion structure would not.
- Wet surfaces overlap `WTR-240`'s wet-terrain darkening and shoreline wetness. These two
  tickets should be designed together or one folded into the other; do not build two
  wetness systems.
- The Water tab's existing `wetHeight` / `wetDarken` controls are the shoreline damp band
  and are a precedent for how a rain wetness term would be exposed and tuned.

Not scoped here: puddles, splash particles, rain audio, windscreen effects, or lightning.
Those are candidates once the occluded-rain core exists.

## WTR-240 — Shoreline contact, wetness and waterline contract — CONDITIONAL_DEPENDENCY, INVESTIGATE

Origin: same review; renumbered from the proposal's `WTR-160` (*Weather and water-body integration* in the Master Plan). `WTR-220` already covers coast **inputs** — shore distance, bathymetry, slope, material. This ticket covers the **rendering and contact** side, which is absent.

Define how water visually and semantically meets terrain, structures, vehicles and characters: shoreline foam, wet terrain darkening, object waterlines, temporary wetness after leaving water, shallow-depth blending, wave run-up, and steep or vertical geometry.

- [ ] No persistent floating shoreline seam.
- [ ] Wetness and foam never alter collision or authoritative gameplay state.
- [ ] Original terrain and materials stay immutable; derived shoreline data is versioned and rebuildable.
- [ ] Ocean, river and shallow-water domains share one engine-facing contract while keeping specialised implementations.
- [ ] Missing shoreline data falls back to the existing water path.

Do not build a universal wet-material framework until at least two production consumers prove the need.

## WTR-250 — Water interaction event interface — CONDITIONAL_DEPENDENCY, IMPLEMENT after WaterQuery migration is stable

Origin: same review; renumbered from the proposal's `WTR-170` (*Gameplay, buoyancy and physics* in the Master Plan). **A water interaction system already ships** — `water/interaction.rs`, `water/interaction.wgsl`, `docs/water-interaction-emitters.md`, and Master Plan phases `WTR-060`/`WTR-070`. Treat this as hardening that system into a bounded interface, never as a second implementation.

**Trimmed to the novel part.** Emitters, wakes, splashes and the submission path already work; re-specifying them would produce a second implementation of a shipping system. The genuinely absent dimension is **multiplayer semantics** — the existing interaction path is single-machine and cosmetic, with no notion of authority, replay or determinism. Restrict this ticket to giving the shipping system those semantics.

- [ ] Every event carries a stable ID and simulation tick, so replaying the same event twice leaves water state identical.
- [ ] Each event is classified gameplay-authoritative or cosmetic, and the classification is enforced: a cosmetic event can never change a `WaterQuery` result.
- [ ] Authoritative events are server-issued or deterministically reconstructible from the event journal (`NET-003`).
- [ ] A late-joining client reaches equivalent visible water state without replaying historical ripples.
- [ ] Events targeting an inactive or unknown water domain are dropped without error.
- [ ] Debug tools list recent events with source entity and authority classification.
- [ ] Event throughput is bounded by an explicit cap, with the drop policy recorded rather than left to chance.

Do not redesign the emitter or solver path, and do not build a universal fluid-event system.

## WTR-260 — Water temporal, lifecycle and invalidation rules — NON_BLOCKING_VALIDATION, VALIDATE

Origin: same review; renumbered from the proposal's `WTR-180` (*FFT and GPU optimisation* in the Master Plan). Covered by neither namespace today — a clean gap.

Define what happens to water histories and derived resources across camera cuts and teleports, resize and minimise, quality changes, backend changes, world or mission changes, device loss and renderer restart, origin changes, water-body creation/destruction/streaming, and bathymetry changes.

- [ ] Foam, reflection and simulation history never leak between unrelated worlds or water bodies.
- [ ] Invalid history is cleared or safely reconstructed.
- [ ] Repeated restart and mission-transition tests do not leak resources.
- [ ] Water resources identify their owner, generation and invalidation reason.
- [ ] Composes with the project-wide temporal and spatial-invalidation contracts rather than inventing parallel lifecycle rules.

## TEST-WTR-001 — Water reference and conformance pack — VALIDATE

Origin: same review; no identifier collision. Extends `TEST-002` rather than adding a parallel capture system. `BLOCKING` only for milestones that release activated water scope.

Fixtures: calm ocean; windy ocean; shore at ground level; shore from the air; underwater and waterline cameras; one river or flowing-water fixture; one small non-ocean body; a boat wake or representative interaction; a projectile crossing the surface; resize, minimise and restart; late join with authoritative water state active.

- [ ] Screenshots with approved visual tolerances.
- [ ] CPU and GPU timings against a named platform tier.
- [ ] Water-body and backend identity recorded.
- [ ] Query-versus-rendered-height error where applicable.
- [ ] Foam/history reset state.
- [ ] Active-domain count and memory.
- [ ] Multiplayer revision and reconstruction result.

## WTR-230 — Multiplayer — REQUIRED

Server authoritative:

- Water level.
- Gameplay current.
- Flood extent.
- Coarse domain parameters and revision.
- Buoyancy outcomes.
- Major impulses.
- Terrain-water coupling.

Preferred replication:

```text
authoritative parameters and events
+ periodic regional snapshots
+ checksums
+ sparse corrections
```

- [ ] Continuous full-grid replication is not the default.
- [ ] Full regional state is an evidence-based snapshot or recovery path.
- [ ] Fine FFT detail, foam, spray, ripples, reflections, and caustics remain derived/cosmetic.
- [ ] GPU simulation is not assumed bit-identical across hardware.
- [ ] Late join receives current parameters, required active-domain snapshots, and revisions.

# 11. Physics, ballistics, and smoke

## PHY-GATE-0 — Physics-backend evidence gate — INVESTIGATE before external-backend commitment

The roadmap does not mandate Jolt, Box3D, or any other physics engine. The required outcome is a defensible decision for the activated Poseidon scope, based on representative workloads, integration risk, multiplayer consequences, and upstream maturity.

Current candidates to reverify when this ticket is activated:

- **Jolt Physics** — current mature baseline candidate with broad rigid-body, query, character, ragdoll, vehicle, tracked-vehicle, heightfield, mesh, buoyancy, multithreading, and deterministic-simulation support. Official sources: https://github.com/jrouwe/JoltPhysics and https://jrouwe.github.io/JoltPhysics/
- **Box3D** — serious emerging candidate with a portable C17 API, heightfields, triangle meshes, baked compounds, large-world double positions, cross-platform determinism, and recording/replay. As of 2026-08-01 its official documentation identifies version 0.1.0 and its author describes it as alpha software, so maturity and missing features must be reassessed. Official sources: https://box2d.org/documentation3d/ and https://box2d.org/posts/2026/06/announcing-box3d/
- **Current Poseidon path or another backend** — valid candidates when they outperform, reduce risk, or better preserve compatibility. Agents may add alternatives when they can state a concrete Poseidon advantage.

This list is not a preselection. Historical feature summaries are evidence inputs, not substitutes for current verification.

Representative comparison workloads should include, where applicable:

- Large WRP heightfield and extreme-coordinate tests.
- Dense town and forest collision built from cooked static compounds.
- Background region loading, insertion, removal, sleeping, and wake-up.
- Parallel ray, sphere, capsule, and overlap queries.
- High-speed bullets, shells, continuous collision, and water transitions.
- Characters on slopes, stairs, doors, moving platforms, and dense triangle terrain.
- Ragdolls, falling trees, debris, wheeled vehicles, tracked vehicles, and buoyancy when those scopes are active.
- Terrain deformation or destruction invalidating regional collision.
- Dedicated-server execution without rendering.
- Snapshot, restore, late join, correction, and cross-platform checksum tests.

Required evidence:

- Median, p95, p99, and worst-case CPU cost.
- Memory use, cooked-data size, cooking time, load time, and background-thread behaviour.
- Query throughput, CCD reliability, contact stability, ghost-collision behaviour, and sleep/wake behaviour.
- Large-world precision and relationship to `COORD-000`.
- Determinism limits, callback ordering, snapshot/restore constraints, and network integration complexity.
- Debug tooling, integration code size, ABI/FFI burden, upstream maintenance, licence, maturity, and local-patch cost.
- Missing features and the cost of implementing or deferring them.

Decision rules:

- [ ] Use `PHY-005` so candidate backends run the same versioned corpus within the same activated conformance profiles.
- [ ] Benchmark the current Poseidon path first, then the strongest mature candidate for the activated scope.
- [ ] Admit an additional challenger only when its capabilities, architecture, maturity trajectory, or measured advantages could realistically change the decision. Exhaustive adapter construction is not required.
- [ ] Record `outcome` separately from `selected_option`; valid selected options include Jolt, Box3D, the current path, another backend, or no external backend for the current scope.
- [ ] Query-only work may proceed through `PHY-000` while dynamic-backend selection remains deferred, when the current path satisfies the active release outcome.
- [ ] Do not select Box3D merely because it is newer or Jolt merely because it is mature.
- [ ] Do not reject an emerging backend solely because it lacks features outside the activated profile set.
- [ ] Prefer one authoritative gameplay backend. A split-backend design requires measured necessity, a conformance boundary, duplicated-cooking and maintenance costs, and a formal disagreement-resolution plan.
- [ ] The decision remains revisitable through `PHY-007` reassessment triggers.

## PHY-005 — Profile-based backend-neutral physics conformance corpus — REQUIRED for backend comparison

Create one versioned corpus whose tests are grouped into independently activatable capability profiles. Every candidate adapter runs the same tests, geometry, tolerances, measurements, and result schema for each profile it is actually competing in.

Initial profile vocabulary:

```text
PHY_QUERY_STATIC
PHY_PROJECTILE
PHY_CHARACTER
PHY_DYNAMIC_BODY
PHY_VEHICLE
PHY_DESTRUCTION
PHY_BUOYANCY
```

`PHY_WATER_BUOYANCY` is a superseded historical alias for `PHY_BUOYANCY`. Existing evidence may retain the old name when it also records the corpus version and alias mapping; new records should use `PHY_BUOYANCY`.

These profiles are starting categories, not permanent architecture. A stronger agent may split, merge, rename, or add profiles when it documents why the revised grouping produces fairer or more representative evidence without weakening comparability.

Profile intent:

- **`PHY_QUERY_STATIC`** — terrain and static-building cooking, ray/shape/overlap queries, material lookup, streaming insertion/removal, extreme coordinates, query determinism, and query-only server cost.
- **`PHY_PROJECTILE`** — high-speed ray/sphere/capsule sweeps or equivalent collision queries, CCD where applicable, material and body identity, penetration/ricochet inputs, water entry and exit, deterministic impact reduction, and dedicated-server cost. It does not require the middleware to own the full projectile integrator when Poseidon retains custom integration.
- **`PHY_CHARACTER`** — capsules or equivalent character shapes, slopes, stairs, doors, moving platforms, dense triangle terrain, depenetration, grounding, and multiplayer correction inputs.
- **`PHY_DYNAMIC_BODY`** — rigid-body contacts, CCD, sleeping, activation, stacks, falling trees, debris-scale workloads, constraints needed by the activated scope, and snapshot/restore.
- **`PHY_VEHICLE`** — wheeled and tracked vehicle requirements, suspension, contacts, terrain interaction, determinism, correction, and server cost.
- **`PHY_DESTRUCTION`** — collision-state replacement, rubble or fragment bodies where applicable, regional invalidation, event reduction, save/load, and late join.
- **`PHY_BUOYANCY`** — buoyancy queries or forces, water-current inputs, stable transitions, sleeping/wake behaviour, correction, and interaction with the selected water representation. The profile may cover boats, floating debris, characters, or other activated buoyant-body scopes without prescribing one solver.

Activation rules:

- [ ] Activate only the profiles needed by the current release, investigation, or implementation decision.
- [ ] The first general query comparison normally activates `PHY_QUERY_STATIC` only.
- [ ] Activate `PHY_PROJECTILE` as well when a candidate is proposed to supply projectile collision, CCD, impact queries, or projectile-related semantic events for the current scope.
- [ ] A candidate is not required to implement vehicle, destruction, character, dynamic-body, projectile, or buoyancy support outside the profiles it is actually competing in.
- [ ] Unsupported profiles remain visible in the result bundle with a reason and are not silently treated as either a pass or a failure.
- [ ] A backend selected for one profile does not automatically win later profiles.
- [ ] Later profile activation may confirm the current backend, justify adapter extensions, trigger reassessment through `PHY-007`, or show that the current selection no longer fits the expanded scope.
- [ ] Prefer reusing one adapter across profiles when appropriate, but do not distort a profile merely to preserve a previous selection.

Every profile definition should specify:

- Input geometry, material semantics, coordinate ranges, and stable object identifiers.
- Queries or simulation steps, event inputs, snapshot points, and expected result sets.
- Exact requirements where gameplay needs exact identity or ordering.
- Approved numerical tolerances where floating-point equivalence is sufficient.
- Performance scenes, cold/warm loading conditions, and platform fingerprints.
- Determinism, snapshot, restore, late-join, and correction expectations where applicable.
- Required versus optional backend capabilities.
- Expected fallback behaviour for unsupported or degraded operation.
- A machine-readable result bundle suitable for independent comparison.

Minimum result metadata:

```yaml
corpus_version: 1
adapter_version: 1
backend_name: example
backend_version: example
activated_profiles:
  - PHY_QUERY_STATIC

conformance_results:
  PHY_QUERY_STATIC:
    result: PASSED_WITH_ADAPTER_WORKAROUND
    evidence:
      - artifacts/physics/query-static-results.json
    workaround:
      description: Poseidon-side canonical sorting of broadphase results
      maintenance_cost: LOW

unsupported_profiles:
  - profile: PHY_PROJECTILE
    result: OUT_OF_SCOPE
    reason: Candidate is not proposed to own projectile collision in this comparison
  - profile: PHY_VEHICLE
    result: OUT_OF_SCOPE
    reason: Not required for the activated query-only comparison
```

Allowed per-profile results:

```text
PASSED
PASSED_WITH_ADAPTER_WORKAROUND
FAILED
NOT_TESTED
OUT_OF_SCOPE
```

A workaround result must record:

- What Poseidon-side behaviour is required.
- Whether the workaround affects runtime cost, cooking, determinism, save/network state, or maintenance.
- Estimated maintenance cost such as `LOW`, `MEDIUM`, or `HIGH`, with supporting rationale.
- Whether the workaround is generic engine infrastructure or backend-specific debt.
- Whether removing the workaround would change the backend decision.

Fairness and evolution rules:

- [ ] Candidate adapters do not silently receive easier geometry, looser tolerances, different measurements, or different activated-profile workloads.
- [ ] Backend-specific optimisation is allowed when the semantic workload remains equivalent.
- [ ] Missing, failed, unsupported, and out-of-scope cases remain visible rather than being removed from the comparison.
- [ ] The corpus may evolve when a stronger agent identifies a more representative Poseidon workload, with a versioned decision record and rerun requirements.
- [ ] Corpus changes do not invalidate historical results silently; result bundles name the exact corpus and adapter versions.
- [ ] An adapter that passes through substantial Poseidon-side replacement logic is not recorded as an unqualified backend pass.
- [ ] Independent review verifies that each activated profile was applied consistently across candidates.

## PHY-006 — Semantic physics-event reduction — REQUIRED before backend events become authoritative gameplay

Physics libraries may emit contacts, manifolds, activations, query results, and constraint callbacks from multiple threads and in backend-specific order. Most of that data is solver detail or implementation noise. Gameplay, networking, saves, and replay must not consume raw callback streams as authoritative state.

Required outcome:

1. Keep raw contact/manifold data inside the backend adapter, solver response, and diagnostics unless an explicit gameplay contract needs selected information.
2. Identify gameplay-relevant state changes or interactions for the authoritative tick.
3. Convert backend handles to Poseidon stable IDs and validate handle generations.
4. Reduce one or many raw callbacks into backend-neutral semantic events or authoritative state transitions.
5. Quantise, normalise, aggregate, or select values only where the gameplay contract requires it.
6. Canonically order semantic events by explicit event class, stable entity IDs, source command or transaction, and documented tie-breakers.
7. Merge duplicate semantic outcomes, reject stale events, and submit only the reduced result to gameplay, networking, saves, and replay.

Illustrative semantic events include:

```text
ProjectileImpact
ProjectileEnteredWater
BodyEnteredWater
BodyExitedWater
CharacterGrounded
CharacterUngrounded
VehicleGroundContact
DestructibleImpulse
BodyActivated
BodySlept
```

These names are examples, not a mandatory universal event bus. A stronger design may use authoritative state transitions, subsystem-specific reducers, or another representation when it preserves the same backend-neutral semantics and evidence requirements.

- [ ] Semantic event schemas are backend-neutral, versioned, and owned by Poseidon rather than by middleware callback types.
- [ ] Raw contacts are not replicated, saved, or replayed merely because the backend exposes them.
- [ ] One semantic event may aggregate many raw callbacks; one raw callback may produce no gameplay event.
- [ ] Contact points, normals, impulses, material IDs, and feature IDs are included only when a consumer contract justifies them.
- [ ] Physics response may still use raw solver data locally without exposing it as authoritative gameplay history.
- [ ] Cosmetic events may use a cheaper or backend-specific path when they cannot affect gameplay outcomes.
- [ ] Multithreaded scheduling and callback-order changes do not change authoritative semantic results.
- [ ] Debug tools can display raw backend callbacks, reducer decisions, and final semantic events side by side.
- [ ] Conformance tests verify equivalent semantic results across backend versions and activated `PHY-005` profiles.
- [ ] Sorting is a candidate mechanism, not the required architecture; a stronger deterministic reducer may replace it when it proves equivalent or better.

## PHY-007 — External physics dependency and reassessment record — REQUIRED when middleware is adopted

For each adopted physics dependency, record:

```yaml
upstream_name:
upstream_url:
upstream_version:
upstream_commit:
license:
maturity:
known_blockers:
local_patches:
integration_adapter:
last_reviewed:
reassessment_triggers:
```

Possible reassessment triggers include:

- The selected backend fails a required conformance or Tier 1 test.
- An emerging candidate reaches a relevant maturity milestone or adds a missing required feature.
- Local patches exceed the approved maintenance budget.
- Large-world, determinism, vehicle, character, or streaming requirements materially change.
- Upstream maintenance, licensing, or platform support changes.
- A stronger agent presents new representative evidence.

- [ ] Upgrades rerun the relevant `PHY-005` corpus.
- [ ] Local patches are documented and minimised.
- [ ] Dependency replacement has a migration and save/network compatibility plan.
- [ ] Reassessment does not imply automatic churn; the current backend remains until evidence justifies change.

## PHY-000 — Narrow physics query abstraction — REQUIRED

Use an `IPhysicsWorld`-style boundary for real consumers. Initial operations:

- [ ] Ray cast.
- [ ] Shape cast.
- [ ] Overlap query.
- [ ] Material lookup.
- [ ] Body-transform query.
- [ ] Stable query/body identifiers.
- [ ] Explicit ownership and lifetime.

Do not design an abstraction for every hypothetical backend before a real second backend and consumer require it.

## PHY-010 — Query-only physics integration — SUGGESTED first external-backend step

When the current query path cannot satisfy the active release outcome, implement the smallest accepted adapter behind `PHY-000` for terrain/building collision, ray casts, shape casts, overlaps, ballistics, smoke, audio, navigation, or other real consumers.

- [ ] Use the backend selected or provisionally approved by `PHY-GATE-0`, unless the ticket explicitly validates the current path.
- [ ] Do not expose backend-specific handles or callback order to gameplay.
- [ ] Do not migrate vehicles, characters, ragdolls, or broad dynamic simulation merely to justify the adapter.
- [ ] Preserve a current-path or safe fallback until the new adapter passes the applicable `PHY-005` corpus.
- [ ] A stronger agent may conclude that no new backend is needed for the query-only phase.

## PHY-020A — Static query collision cooking — REQUIRED for Milestone 4

Required for reliable ballistics, smoke collision, and atmosphere queries:

- [ ] Terrain heightfields.
- [ ] Static building geometry.
- [ ] Material IDs.
- [ ] Stable query hashes.
- [ ] Debug visualisation.
- [ ] Only dynamic shapes directly required by current ballistics or smoke consumers.

## PHY-020B — Expanded dynamic collision cooking — LATER

Deferred until dynamic-physics and vehicle work:

- [ ] Authored vehicle compounds.
- [ ] Debris shapes.
- [ ] Broader dynamic props.
- [ ] Advanced character collision.
- [ ] Vehicle-specific collision integration.

## ANIM-000 — Runtime animation and physics bridge — INVESTIGATE as a bounded capability

The required question is how to improve character, weapon, vehicle, and ragdoll motion without breaking existing animation data, mission semantics, networking, or the later asset-compatibility programme. Motion matching, one particular graph architecture, or full procedural animation is not required.

Candidate outcomes and techniques may include:

- Compatibility-preserving animation-state evaluation.
- Foot placement and slope adaptation.
- Hand, weapon, sight, and vehicle-entry alignment.
- Aim offsets, recoil, hit reactions, and layered pose control.
- Partial and full ragdoll transitions with recovery.
- Animation-to-physics and physics-to-animation ownership rules.
- Previous-pose data or an approved approximation for `TEMP-000`.
- Pose replication, interpolation, compression, and late-join state.
- Debug views for skeletons, constraints, ownership, pose sources, and divergence.

- [ ] Capture the current animation baseline before changing semantics.
- [ ] Separate required gameplay pose from local cosmetic refinement.
- [ ] Preserve original RTM/config behaviour or document explicit compatibility limits.
- [ ] Do not make dynamic physics adoption a prerequisite for useful animation improvements.
- [ ] Any ragdoll or physics-driven outcome uses `PHY-006` when events can affect gameplay.
- [ ] Agents may retain, extend, replace, or layer the current animation architecture when evidence supports the choice.

## BAL-010 — Modern projectiles — SUGGESTED

Custom projectile integration with wind-relative velocity, verified drag model, stable integrator, adaptive substeps, ray/sphere casts, materials, penetration, ricochet, fragmentation, and water transition.

## BAL-020 — Server authority — REQUIRED

Server owns projectile spawn, integration, wind, collision, damage, and impact. Clients predict tracers and cosmetic effects only.

## FX-100 — Preserve smoke baseline — REQUIRED before replacement

Capture current appearance, performance, wind, lighting, clipping, and multiplayer behaviour.

## FX-110 — Collision-aware smoke — SUGGESTED immediate improvement

Sweep motion, stop at walls, remove inward velocity, preserve tangential flow, add surface turbulence, prevent repeated penetration, and support ceiling pooling.

## FX-120 — Shared world-query interfaces and local-field research — INVESTIGATE

Systems should first share:

- World-space query interfaces.
- Source geometry and semantic material data.
- Regional invalidation events.
- Conversion/cooking infrastructure.

They are not required to share an identical grid, resolution, storage format, or update cadence.

Evaluate whether a shared local SDF/occupancy representation can support smoke, indoor lighting, wind, fog, particles, water obstacles, and audio. Specialised representations are preferred when a universal field would be too coarse, too expensive, or semantically incorrect.

## FX-130 — Local volumetric smoke grids — OPTIONAL

Density, temperature, velocity, obstacles, buoyancy, vorticity, pressure approximation, sleeping, distance LOD, doors/windows, downwash, and vehicle wake.

Gameplay smoke remains a coarse server-authoritative visibility/thermal/toxicity representation; detailed curls may differ.


## AUD-000 — Geometry-aware AudioWorld — SUGGESTED staged capability

Audio should be allowed to consume the same authoritative geometry, materials, weather, water, and interior information as visual systems without forcing a new audio backend or one propagation technique.

Potential stages:

1. Capability audit and optional headphone/HRTF path where supported.
2. Material-aware obstruction and low-pass behaviour using physics queries.
3. Indoor/outdoor and room/portal classification.
4. Environment-dependent reverberation and gunshot tails.
5. Weather, terrain, vegetation, water, and altitude-dependent ambience.
6. Projectile fly-bys, supersonic cracks, underwater transitions, and vehicle/environment coupling.

- [ ] Existing mission and sound behaviour remains compatible.
- [ ] Gameplay-critical sound cues are not hidden by cosmetic quality settings.
- [ ] Query cost and source-count scaling are measured.
- [ ] Missing room, portal, or material data falls back safely.
- [ ] Debug views expose obstruction rays, room/portal state, materials, active effects, and fallback reasons.
- [ ] Agents may prefer portals, probes, ray queries, analytic rules, baked data, or hybrids based on evidence.
- [ ] Basic audio work must not block Preview 1B unless the ledger explicitly promotes it.

---

# 12. Destructible terrain

## TRN-000 — Scope

Initial target: craters, trenches, berms, hills, flattening, ruts, mud, and flood depressions. Initial non-goals: global voxel terrain, caves, tunnels, overhangs, granular soil, and physical deformation from every bullet.

## TRN-GATE-1 — TerrainQuery consumer migration complete — REQUIRED before deformation

`WORLD-001` defines the interface. This gate verifies migration.

- [ ] Physics uses `TerrainQuery`.
- [ ] AI/navigation reads use `TerrainQuery`.
- [ ] Water/bathymetry uses `TerrainQuery`.
- [ ] Ballistics uses `TerrainQuery`.
- [ ] Object placement and foundations use `TerrainQuery`.
- [ ] Rendering and vegetation use it where canonical data is required.
- [ ] Zero-delta behaviour matches the old implementation.
- [ ] No unexplained gameplay-critical direct readers remain.
- [ ] Bypass diagnostics are available.

## TRN-010 — Nondestructive sparse deformation — REQUIRED

```text
Final height = base WRP height + persistent delta + optional temporary delta
```

- [ ] Base terrain remains immutable.
- [ ] Sparse tiles, revisions, checksums, save/load, server authority, late join, and restore-original operation.

## TRN-020 — Deterministic stamps — SUGGESTED

Craters, trenches, berms, hills, flatten, smooth, restore, noise, and ruts. Craters include bowl, rim, irregularity, exposed material, scorch, vegetation removal, and debris seed.

## TRN-030 — Partial updates — SUGGESTED

Immutable base heightmap plus editable delta texture, with dirty updates to height, normals, CDLOD bounds, shadows, grass, wetness, water bathymetry, physics, and navigation.

## TRN-050 — Roads, buildings, navigation fallback, and multiplayer

Investigate road damage, repair, foundations, settling, and collapse.

Safe first navigation fallback:

1. Update authoritative terrain and collision immediately.
2. Mark affected navigation regions dirty.
3. Prevent AI path planning through invalid regions.
4. Rebuild or replace affected navigation data.
5. Restore routing after revision validation.

- [ ] Server sends deterministic terrain edits.
- [ ] Clients track tile revisions and checksums.
- [ ] Mismatch requests an authoritative tile snapshot.
- [ ] Late join receives changed tiles.
- [ ] Navigation-invalid regions and revisions replicate where gameplay requires them.


## SURF-000 — Persistent and transient surface state — INVESTIGATE

Weather, water, vehicles, footsteps, explosions, fire, snow, terrain deformation, and Zeus may all alter how surfaces look and sometimes how they behave. Investigate a coherent representation for surface condition without assuming one universal texture, decal, clipmap, or sparse-tile system.

Candidate channels include:

```text
Wetness
Standing water or puddle tendency
Mud and rut intensity
Snow, frost, or dust
Burn, scorch, soot, and exposed material
Tracks, footprints, and impact marks
Vegetation disturbance
Optional gameplay friction or traversal modifiers
```

The investigation must separate:

- Server-authoritative gameplay state.
- Persistent replicated cosmetic state.
- Deterministically derived state.
- Local transient decals and microdetail.

- [ ] Original assets remain immutable.
- [ ] State has explicit lifetime, revision, save/load, late-join, and fallback rules where applicable.
- [ ] Terrain and object surfaces may use different representations when that is technically superior.
- [ ] Weather and deformation do not each create incompatible wetness, mud, scorch, or track systems.
- [ ] Sparse updates, memory cost, blending order, decal limits, and streaming are measured.
- [ ] A small vertical slice—such as rain wetness plus vehicle tracks or crater scorch—should precede a broad accumulation framework.
- [ ] Rejection or deferral is valid when simpler existing material/decal paths meet the required outcome.

---

## NAV-000 — Navigation and tactical-world query strategy — INVESTIGATE

The required outcome is consistent AI movement and tactical querying as terrain, water, buildings, vegetation, roads, doors, destruction, and Zeus edits change. One universal navmesh is not required.

Questions to answer:

- Which existing AI and navigation paths already work and where do they bypass canonical world state?
- Which problems need long-range hierarchy, local navigation, vehicle-specific routing, water-aware routing, dynamic obstacles, or incremental regional rebuilding?
- Which tactical queries—cover, firing positions, visibility, traversability, formation corridors, embarkation, and garrison positions—should share geometry or remain specialised?
- How should invalid regions behave while data is rebuilding?

Candidate approaches include hierarchical graphs, local navmeshes, grids, portals, flow fields, specialised vehicle graphs, baked hints, runtime patches, or hybrids.

- [ ] `TerrainQuery`, `WaterQuery`, physics geometry, and authoritative revisions remain the source of truth where applicable.
- [ ] The safe invalid-region fallback in `TRN-050` remains valid until a better mechanism is proven.
- [ ] Dynamic updates are regional and budgeted; global rebuilds require evidence.
- [ ] Different rendering quality settings do not alter AI traversability or tactical outcomes.
- [ ] Debug tools expose paths, costs, invalid regions, rebuild queues, cover queries, and revision mismatches.
- [ ] A stronger agent may preserve the current navigation system and add only missing specialised layers.

## DEST-000 — Object and building destruction contract — INVESTIGATE before broad destruction implementation

Terrain deformation does not by itself define object, vegetation, vehicle, or building destruction. The required outcome is an authoritative, saveable, late-join-safe state transition with consistent collision, navigation, cover, rendering, audio, and far-world consequences.

Potential first vertical slice:

```text
explosive impact
→ server-validated damage transition
→ building or object section changes state
→ collision and cover update
→ navigation invalidation or fallback
→ deterministic debris/effect seed
→ save, undo where applicable, and late join
```

Candidate mechanisms include authored damage states, sectional replacement, constrained procedural fracture, detachable components, falling-tree states, sparse debris, or hybrids.

- [ ] Preserve existing damage and mission behaviour unless an explicit compatibility decision says otherwise.
- [ ] Server owns gameplay-relevant destruction state and stable IDs.
- [ ] Debris detail may be cosmetic, but collision and cover outcomes are authoritative.
- [ ] `WORLD-004` or a documented specialised invalidation path updates all real consumers.
- [ ] Streaming and far-world representations do not silently retain destroyed objects.
- [ ] Procedural fracture is optional and must prove value over authored states.
- [ ] Preview 2 does not require broad building destruction unless its release scope explicitly adopts it.

# 13. Far-world renderer and voxel decision gate

A separate Metal-fork voxel landscape with voxel trees was discussed, but its source is currently unavailable, so no implementation dependency can be scheduled against it. Major overlapping far-world work remains on HOLD until `FAR-000`. If that reference implementation becomes available before the formal decision, it must be included in the comparison.

## FAR-000 — Far-world research gate — HOLD overlapping major far systems

Before large far-terrain, far-tree, or far-meadow replacements, compare:

1. Voxel far landscape with voxel canopy/trees.
2. Conventional terrain LOD plus tree impostors.
3. Mesh-cluster/HLOD.
4. Hybrid terrain plus canopy-density volumes.
5. Targeted improvements to the current system.

Evaluate ground/helicopter/aircraft transitions, silhouette, canopy parallax, buildings, shadows, fog, water reflections, GPU time, VRAM, streaming, build time, Vulkan/Metal/DX12 portability, dynamic tree removal, terrain updates, and multiplayer implications.

## FAR-010 — Bounded voxel prototype — INVESTIGATE only if justified

Define a narrow question before coding, for example:

> At approved aircraft distances, can a voxel-canopy representation improve forest silhouette and parallax over the best impostor/HLOD baseline while staying inside approved GPU-time and VRAM limits and preserving acceptable transitions?

The scene, distances, platform tier, time allowance, memory allowance, and prototype scope must be approved first.

If tested, keep it a derived rendering representation, not authoritative terrain, collision, or vegetation. Regenerate dirty bricks locally; do not replicate voxel volumes.

Possible voxel attributes: coverage/opacity, average albedo, normal cone, roughness, material class, canopy density, and optional transmission. Far grass should be coverage data, not individual voxel blades.

## FAR-020 — Formal decision

- [ ] Adopt voxels.
- [ ] Adopt hybrid.
- [ ] Reject voxels.
- [ ] Defer.

Decision requires evidence, timings, memory, portability, transition quality, and a list of roadmap work made redundant.

---

# 14. Arma asset compatibility

## AST-000 — Compatibility levels

1. Static visuals: geometry, LODs, PAA, RVMAT, selections, proxies, shadow/geometry/roadway LODs.
2. Animation: skeletons, RTM, axes, turrets, wheels, memory points.
3. Complete simulation: vehicles, weapons, damage, destruction, suspension, animation sources, physics metadata.
4. Mission/addon behaviour: configs, RAP, SQF, handlers, dependencies, AI, and simulation classes.

Do not promise Level 4 early.

## AST-010 — Extend PoseidonFormats — SUGGESTED

Review existing parsers before creating a second stack. Add legal test files, version detection, error reports, fuzzing, and round-trip tests where possible.

## AST-020 — Offline asset compiler — SUGGESTED

Candidate conversions:

```text
P3D → .pmesh
PAA → KTX2/native cache
RVMAT → .pmat
RTM → .panim
WRP → .pworld
Config → resolved metadata database
```

Include deterministic output, provenance, unsupported-feature reports, collision cooking, material translation, and separate gameplay/visual hashes.

## AST-030 — Modder feedback loop — SUGGESTED

Experienced Arma 2/OA and Arma 3 modders can advise on format edge cases, legal test assets, compatibility tests, unsupported features, and practical workflows. The goal is a compatibility/import layer requiring as little manual conversion as reasonably possible.


## SCR-000 — Script runtime observability and safeguards — SUGGESTED before semantic rewrites

Before replacing scripting semantics or promising broad mission/addon compatibility, improve visibility into the existing script runtime.

Candidate capabilities:

- Per-script, command, function, event-handler, and scheduler timing.
- Scheduled versus unscheduled execution visibility.
- Network locality and authority tracing.
- Allocation, queue, wake-up, and command counts.
- Long-frame and runaway-script detection.
- Development-only breakpoints, traces, or hot reload where safe.
- Links from script activity to authoritative events and flight-recorder captures.

- [ ] Existing mission semantics remain unchanged by default.
- [ ] Instrumentation overhead is bounded and disableable.
- [ ] Diagnostics identify uncertainty rather than attributing costs incorrectly.
- [ ] Safeguards do not silently terminate valid scripts in release builds.
- [ ] Evidence from this ticket should guide later compatibility or scheduler work instead of predetermining a rewrite.
- [ ] A more capable agent may propose static analysis, sampling, tracing, deterministic replay, or other methods when they provide better value.

---

# 15. Zeus in Debug Tools

## Zeus prerequisite

See the canonical `ZEU-000A` definition in Section 5. All later Zeus commands use that substrate rather than creating separate replication, permission, journal, or undo paths.

## ZEU-000 — Extract backend, preserve UI — REQUIRED

Suggested services: `ZeusSession`, `ZeusCommandService`, `ZeusSelectionService`, `ZeusObjectRegistry`, `ZeusTransactionHistory`, `ZeusPermissions`, `ZeusReplication`, and tool modules. The existing ImGui debug UI continues to call them.

## ZEU-010 — Select and edit the whole world

- [ ] Mission and script-spawned entities.
- [ ] Existing vehicles, groups, buildings, modules, and Zeus objects.
- [ ] Bounds-aware picking, box/lasso, group/crew/same-type selection, filters, locking, occluded cycling, gizmos, snapping, and placement preview.

Do not rely only on a local spawned-object list.

## ZEU-020 — Commands and transactions — REQUIRED

Server-validatable commands for spawn, move, rotate, delete, composition copy, AI orders, properties, weather, time, terrain, vegetation, water, and effects.

- [ ] Undo/redo.
- [ ] Named transactions.
- [ ] Server journal.
- [ ] Save/load and late join.
- [ ] Audit log and permissions.

## ZEU-030 — Asset browser and AI

Searchable config catalogue, faction/side/category/mod filters, crewed/empty, favourites, templates, missing-addon warnings, and cost estimates.

AI tools: groups, leaders, waypoints, patrol, guard, hold, search/destroy, retreat, embark, garrison, artillery, formation, behaviour, combat mode, skill, morale, and rules of engagement. Placed AI resumes normal simulation unless explicitly static.

## ZEU-100 — Weather and time

Weather presets, overcast, rain/snow, fog, wind/gusts, storm/cloud controls, transition duration, date/time, multiplier, pause, and dawn/noon/sunset/midnight. All are authoritative commands connected to the unified weather system.

## ZEU-110 — Terrain, vegetation, water, and environment

Terrain: crater, raise/lower, smooth, flatten, hill, trench, berm, restore, material, wetness, mud, snow, burn/remove/restore vegetation.

Water: river spline, lake polygon, source, drain, flow modifier, flood, waterfall, rapids, domain debug, bake/sleep.

Environment: fire, smoke, fog volumes, wind/turbulence volumes, snow, mud, puddles, rubble, destruction, roadblocks, and fortifications.

Workflow: local preview → compact command → server validation → authoritative edit → regional invalidation → undoable transaction.

## ZEU-120 — Modules, cinematics, and permanent debug controls

Modules: objectives, triggers, end conditions, respawn, capture/extraction, reinforcements, artillery/CAS, mines, garrisons, civilians, supplies, weather fronts, floods, and fire.

Cinematics: camera bookmarks/paths, unit attachment, DoF, slow motion, time/weather keyframes, possession, replay markers, and screenshot presets.

Permanent debug controls: raw config spawn, network locality/ownership, IDs, physics shapes, AI/nav state, dirty terrain tiles, water domains, wind vectors, checksums, packet simulation, forced resync, and resource budgets.

---

# 16. Release slices and revised milestone order

## Milestone checklist interpretation — REQUIRED governance

A ticket appearing under a milestone identifies its intended scheduling context; it does **not** automatically make the ticket a release blocker. The authoritative ledger assigns `milestone_role`, and the explicit preview or milestone exit criteria determine what must pass before shipment.

Initial roles for the current capability domains are:

| Ticket | Initial milestone role | Scheduling intent |
|---|---|---|
| `REL-000` | `BLOCKING` | Required for the public Preview 0 package after the technical Build Truth candidate passes. |
| `RND-030` | `BLOCKING` | Reconcile existing renderer work before overlapping renderer implementation. |
| `FRAME-000` | `NON_BLOCKING_VALIDATION` | Measure frame pacing and input-to-presentation latency early; promote individual fixes only when measured release problems justify it. |
| `DBG-REC-000` | `OPTIONAL_PARALLEL` | Add a small diagnostic bundle when existing evidence sources make it cheap; do not delay foundations. |
| `TEMP-000` | `NON_BLOCKING_VALIDATION` | Establish minimum history-lifecycle rules where needed; do not require a grand temporal framework for ocean closure. |
| `COORD-000` | `NON_BLOCKING_VALIDATION` | Investigate precision early; its accepted findings become a dependency of `FAR-000`, not a Milestone 1 release blocker. |
| `STR-000` | `CONDITIONAL_DEPENDENCY` | Becomes blocking only before an activated HD, retained-scene, or far-world expansion that requires a residency strategy. |
| `STR-010` | `CONDITIONAL_DEPENDENCY` | Implement only after an adopted or adapted `STR-000` decision and only when the selected scope is needed. |
| `AST-006` | `BLOCKING` | Required only for Compatibility Preview C0; it does not imply broader later-generation gameplay compatibility. |
| `WTR-200` | `NON_BLOCKING_VALIDATION` | Activate only when a shallow-water decision is needed for an approved release or experiment. |
| `WTR-210` | `CONDITIONAL_DEPENDENCY` | Required only when an adopted water approach needs nested domains, baking, or sleeping. |
| `WTR-220` | `OPTIONAL_PARALLEL` | Expanded river/coast integration occurs after the narrow WaterQuery proof unless explicitly promoted. |
| `WORLD-004` | `CONDITIONAL_DEPENDENCY` | Use when at least two real consumers require a shared invalidation contract; specialised invalidation remains acceptable. |
| `SURF-000` | `OPTIONAL_PARALLEL` | Does not block Preview 2 unless its accepted release scope explicitly includes persistent surface state. |
| `LIT-030` | `OPTIONAL_PARALLEL` | May investigate indirect lighting without delaying Preview 1A. |
| `AUD-000` | `OPTIONAL_PARALLEL` | May begin with a bounded audio slice after query geometry exists. |
| `PHY-GATE-0` | `CONDITIONAL_DEPENDENCY` | Becomes blocking before committing to an external or replacement physics backend; query-only work may defer it when the current path satisfies the active release. |
| `PHY-005` | `CONDITIONAL_DEPENDENCY` | Required only for the activated conformance profiles of a comparative backend decision; later profiles remain out of scope until separately activated. |
| `PHY-006` | `CONDITIONAL_DEPENDENCY` | Required before backend-generated events become authoritative gameplay. |
| `PHY-007` | `CONDITIONAL_DEPENDENCY` | Required when external physics middleware is adopted or upgraded. |
| `ANIM-000` | `OPTIONAL_PARALLEL` | May improve animation and physics transitions without blocking current previews. |
| `NAV-000` | `NON_BLOCKING_VALIDATION` | Investigate navigation consistency during editable-world work; the documented invalid-region fallback remains acceptable. |
| `DEST-000` | `OPTIONAL_PARALLEL` | Object/building destruction does not block Preview 2 unless explicitly adopted into its release scope. |
| `SCR-000` | `NON_BLOCKING_VALIDATION` | Gather evidence before approving deeper script-runtime changes. |

The ledger may change these roles when implementation evidence justifies promotion or demotion. Any promotion to `BLOCKING` requires an approved decision record.

## Preview 0 — Trustworthy WGPU build

Ships at the end of Milestone −1.

- [x] An original mission loads with confirmed WGPU.
- [x] Build fingerprint and adapter/backend report are available.
- [x] CI validates composed WGSL and ABI compatibility.
- [x] Silent GL33 fallback is impossible in the WGPU startup test.
- [x] Missing optional content does not prevent startup.
- [x] Timings and at least one reproducible capture are produced.
- [x] Deliberate, explicit GL33 fallback remains available.

Preview 0 requires optional-content startup safety, not the complete HD package system.

## Compatibility Preview C0 — Cross-generation content proof

Ships after the Asset and Material Spine when `AST-006` passes.

```text
OFP/CWA baseline fixture
+ representative later-generation static and animated fixtures
→ explicit version detection
→ deterministic derived conversion/cache
→ structured unsupported-feature report
→ reference-scene rendering or precise rejection
→ cache removal and clean rebuild
```

- [ ] Every fixture is legally usable for the selected test arrangement.
- [ ] Original data remains immutable.
- [ ] Static visuals, animation, simulation semantics, and mission/addon behaviour are reported separately.
- [ ] Failure to support an advanced feature produces an actionable report rather than a silent incorrect import.
- [ ] The preview does not claim drop-in later-generation mission, config, vehicle, or script compatibility.
- [ ] The published capability matrix describes exactly which fixture features passed.

This preview is an architectural proof that PoseidonWGPU is progressing toward later Real Virtuality content, not merely improving OFP rendering.

## Preview 1A — Coherent visual atmosphere

Ships at the end of Milestone 3.

```text
Zeus weather command
→ authoritative WeatherState
→ client reconstruction
→ clouds
→ cloud-shadow response through CLD-020 or an approved fallback
→ lighting and god rays
→ grass and preserved downwash
→ water-surface visual response
```

- [ ] Two clients agree on authoritative weather.
- [ ] Late join reconstructs the transition.
- [ ] Visual quality may differ without gameplay divergence.
- [ ] Performance fits named platform budgets.

## Preview 1B — Coherent gameplay weather

Ships at the end of Milestone 4.

```text
WeatherState and AtmosphereQuery
→ ballistic wind
→ smoke advection
→ at least one representative local airflow integration
→ gameplay authority
→ correction and late join
```

- [ ] Ballistics sample authoritative wind consistently.
- [ ] Gameplay smoke uses an authoritative coarse representation.
- [ ] Local visual detail may differ safely.
- [ ] Packet-loss, reconnect, and late-join tests pass.

## Preview 2 — Editable-world slice

Ships at the end of Milestone 6.

```text
Zeus crater
→ server validation and transaction
→ terrain delta
→ rendering and collision
→ vegetation removal
→ water response
→ AI/navigation invalidation
→ save
→ undo
→ late join
```

- [ ] Server and clients agree on tile revision/checksum.
- [ ] Missing HD assets do not affect gameplay.
- [ ] Undo and replay are idempotent.
- [ ] Reconnected clients receive correct state.
- [ ] Navigation uses the safe invalid-region fallback until rebuilt.

## Milestone −1 — Build Truth

- [ ] PERF-001 initial support tiers declared.
- [ ] CORE-NEG-001.
- [ ] CORE-NEG-002.
- [ ] RND-005A.
- [ ] CORE-005.
- [ ] TEST-002 minimum build fingerprint and capture support.
- [ ] REL-000 after the technical Preview 0 candidate is reproducible.

**Exit criterion:** Public Preview 0 shipped with a downloadable or reproducible release package and honest capability matrix.

### Initial execution activation — REQUIRED

Do not activate the whole roadmap at project start. Before Preview 0, the execution overlay should authorise only the minimum Build Truth set. Authorisation is not activation: the live ledger changes a ticket to `ACTIVE` only after an owner successfully claims it.

The initial queue input is:

```yaml
- id: PERF-001
  initial_scheduling_state: OPEN
  size: SMALL
  cross_system: false
  active_scope: one realistic Tier 1 declaration sufficient to define Preview 0 gates

- id: CORE-005
  initial_scheduling_state: OPEN
  size: SMALL
  cross_system: false
  active_scope: bootstrap-to-full ledger schema, duplicate/dependency checks, and evidence contract

- id: CORE-NEG-001
  initial_scheduling_state: BLOCKED
  initial_blocked_by: [PERF-001]
  size: LARGE
  cross_system: true

- id: CORE-NEG-002
  initial_scheduling_state: OPEN
  size: LARGE
  cross_system: true

- id: RND-005A
  initial_scheduling_state: OPEN
  size: LARGE
  cross_system: true

- id: TEST-002
  initial_scheduling_state: OPEN
  size: SMALL
  cross_system: false
  active_scope: minimum build fingerprint, backend proof, timing export, and one reproducible capture
```

A bootstrap process may create missing ledger records from these initial values, but it must not overwrite an existing record. The ledger remains the only live authority after creation.

`REL-000` remains unauthorised and blocked by the six technical Build Truth entries. Authorise it only when the Preview 0 candidate can be reproduced from the recorded build inputs. Its canonical role remains `BLOCKING` for the public Preview 0 package.

Everything else remains non-active until the integration owner explicitly authorises it through the overlay and claims it through the ledger. Tickets may remain `PLANNED` with scheduling state `OPEN`, `HOLD`, or `BLOCKED`; `DEFERRED` should be used only after an explicit decision to postpone work rather than merely because it has not started.

When more than one agent works on Preview 0, activate only the minimum applicable controls from `CORE-006`: one owner per ticket, one integration owner, recorded baseline commits, coordination of ABI changes, and independent verification before `SHIPPABLE`. Full completion of `CORE-006` belongs to Milestone 0A.

## Quick Wins lane — begins after Preview 0

- [ ] QW-000 governance active.
- [ ] Candidates selected from measured production issues.
- [ ] Quick Wins run in parallel and do not block milestone gates.
- [ ] Architectural work is not relabelled as a Quick Win.
- [ ] Every merged Quick Win has evidence and independent review.

## Milestone 0A — Minimum execution foundation

- [ ] CORE-000, scoped to active systems.
- [ ] TEST-001.
- [ ] TEST-002 complete.
- [ ] NET-001.
- [ ] NET-002.
- [ ] Minimal NET-003 event envelope.
- [ ] ZEU-000A.
- [ ] CORE-006.
- [ ] RND-000.
- [ ] RND-010.
- [ ] `DBG-REC-000` minimal capture path where it can reuse existing evidence sources without delaying the foundation.
- [ ] `WORLD-004` introduced only when multiple real consumers require a shared invalidation contract.
- [ ] WORLD interfaces introduced as first consumers are scheduled.

This lane must not become a multi-year prerequisite.

## Milestone 0B — Expanded multiplayer validation

Develop alongside the first authoritative vertical slices:

- [ ] TEST-003 through TEST-006.
- [ ] NET-003 completed and versioned.
- [ ] NET-004 snapshots/checksums.
- [ ] NET-005 fault and late-join harness.
- [ ] NET-006 schema migration.
- [ ] General reconstruction APIs proven by real systems.

## Milestone 1 — Renderer stabilisation

- [ ] WTR-001.
- [ ] WTR-GATE-1.
- [ ] TEST-GRS-001.
- [ ] TEST-ZEU-001.
- [ ] RND-020.
- [ ] RND-021 only if RND-020 outcome is `ADOPTED` or `ADAPTED`.
- [ ] RND-030 renderer-plan reconciliation.
- [ ] FRAME-000 measures frame pacing and latency; unresolved work remains non-blocking unless a specific release regression is promoted.
- [ ] TEMP-000 produces an evidence-backed temporal-history decision and minimum lifecycle contract.
- [ ] COORD-000 establishes whether large-world precision changes are needed before far-world expansion.
- [ ] RND-005B.
- [ ] GL33-010 where approved.

**Exit criteria:**

- [ ] Renderer lifecycle recovery is either validated or has a documented safe fallback.
- [ ] Ocean closure evidence is current.
- [ ] Qualified Quick Wins may continue independently.

## Milestone 2A — Asset, material, and compatibility proof

- [ ] AST-005.
- [ ] MAT-000 and MAT-010.
- [ ] PAK-000 through PAK-020.
- [ ] ASSET-010.
- [ ] AST-006 cross-generation fixture proof.

**Exit criterion:** Compatibility Preview C0 shipped with a legal fixture corpus, deterministic derived-cache path, and explicit compatibility report.

Compatibility Preview C0 must not be delayed merely because broader grass, HD-library, or residency work remains unfinished.

## Milestone 2B — Grass package and bounded residency work

- [ ] GRS-GATE-1.
- [ ] One complete grass package path.
- [ ] STR-000 produces an evidence-backed residency decision before large HD or retained-scene expansion.
- [ ] STR-010 only if STR-000 outcome is `ADOPTED` or `ADAPTED` and the selected scope is needed now.
- [ ] No broad species-library expansion before the gate passes.

Milestone 2B may overlap Compatibility Preview C0 packaging when ticket ownership and WIP limits allow it, but it must not broaden the C0 compatibility claim.

## Milestone 3 — Coherent visual atmosphere

- [ ] WORLD-003.
- [ ] ATM-GATE-1.
- [ ] ATM-000 and ATM-010.
- [ ] CLD-010.
- [x] CLD-020 — shipped 2026-08-05 as the approved cheaper fallback (a marched sun-transmittance
      map, not clipmaps). All five fallback criteria met and recorded; cost below noise.
- [ ] CLD-030.
- [ ] GRS-040 downwash preservation.
- [~] LIT-010 — shipped and default ON, but its cost is not settled: ~8% of the GPU frame at
      800x600, and **31.9%** re-measured in the reference mission at the Tier 1 resolution. Needs
      a quality/cost decision. See LIT-010 above.
- [~] LIT-020 — both stages implemented and default ON as of 2026-08-05, including the per-model
      baked sky-visibility volumes. Two things keep this open rather than closed: the owner has not
      re-judged it since the bake landed, and the §3d hardening (content-hashed cache, background
      scheduling, model-variance policy) does not exist. Its cost is measured and modest —
      0.570 ms per frame, and 8 models / ~0.29 s of bake in the reference mission, correcting an
      earlier ~10 s estimate that assumed the whole model library. See LIT-020 above.
- [x] Grass silhouette work (2026-08-05, tester-driven). `species_shape()` returned THREE profiles
      for eight species, so all four grass species were geometrically identical and a field read as
      one blade repeated. Now eight distinct profiles with per-blade taper and bend jitter. A second
      pass found the deeper fault: the taper EXPONENTS were far too high, making every blade a
      spear (0.65 is 64% width at mid-height), and the bend was an absolute distance of 5-19 cm —
      about ten degrees — so the field stood to attention. Retuned to 0.09-0.46 and arch is now
      proportional to blade height. Costs nothing; it is vertex-shader width and curve.
- [x] Alpha cut-out card path for near grass, off by default. Silhouette from texture alpha rather
      than geometry. Measured +0.390 ms on the grass colour pass when enabled — and the measurement
      that matters more: putting the cutout behind a runtime flag INSIDE the shader cost +67% with
      the feature switched OFF, because a `discard` anywhere disables early-Z. Split into its own
      entry point and pipelines.
- [ ] LIT-030 may investigate indirect-light options but does not block Preview 1A.
- [x] Night sky: stars (2026-08-05). Procedural field gated to night, brightness slider in the Sky
      tab. Owner-confirmed visible. Two gaps it exposed, both open: clouds do NOT occlude the stars
      despite being composited after them — that ordering argument was wrong and needs re-testing,
      not re-asserting — and real constellations need a star catalogue, which is a data job. The
      moon remains undone.
- [~] Water planar reflection edge (open, parked 2026-08-06). Looking down at water still shows a
      boundary where the cloud reflection stops. Three fixes landed, each correcting a real fault
      and none closing this: the reflected camera inherited the screen's exact FOV; the handover to
      the sky reflection ran over 3% of the target; and the fade measured the wrong distance, so it
      discarded reflection over water the target covers. It now measures the overshoot and
      dissolves outward through the mip chain. Reduced, still visible. Settle whether the remainder
      is the planar cutoff or Fresnel (debug view 23) BEFORE touching the fade again.
- [x] Player water ripples work. A report that they had stopped was a STALE DEPLOY — the running
      binaries did not match the build. Content-checking the deployment is what settled it, which is
      the same lesson 36a9d29 already recorded. Owner's remaining note: they could be stronger.
- [ ] TEMP-000 rules are applied to new cloud, god-ray, reflection, or volumetric histories where relevant.
- [ ] Zeus weather/time commands through ZEU-000A.

**Exit criterion:** Preview 1A shipped.

## Milestone 4 — Gameplay weather, physics queries, and effects

- [ ] PHY-000 provides the narrow query boundary needed by real consumers.
- [ ] The active path supplies reliable terrain/building queries for ballistics, smoke, atmosphere, and other Preview 1B consumers.
- [ ] PHY-GATE-0 is activated only when external-backend commitment is needed now; otherwise record the verified current path and defer the broader decision.
- [ ] `PHY-005` normally activates `PHY_QUERY_STATIC` for the first general query comparison and also activates `PHY_PROJECTILE` when the candidate is proposed to serve projectile collision or impact semantics; PHY-006, PHY-007, PHY-010, and PHY-020A apply according to the selected path and ledger dependencies rather than automatically blocking the milestone.
- [ ] BAL-010 and BAL-020.
- [ ] FX-100 through FX-120.
- [ ] ATM-020 representative integration.
- [ ] Smoke and ballistics consume Weather/AtmosphereQuery.
- [ ] AUD-000 may begin with a bounded geometry-occlusion or HRTF slice; it is non-blocking unless promoted in the ledger.

**Exit criteria:**

- [ ] Preview 1B shipped.
- [ ] Query and collision behaviour required by Preview 1B is validated regardless of backend choice.
- [ ] Full vehicle, character, ragdoll, and broad dynamic-physics migration remains out of scope.

## Milestone 5 — Narrow WaterQuery interoperability proof

The purpose of this milestone is to prove that the existing ocean and one simple non-ocean body can share a trustworthy engine-facing query and multiplayer contract. It is not permission to build the full future water system before editable-world and compatibility work can proceed.

- [ ] WORLD-002.
- [ ] WTR-GATE-2.
- [ ] WTR-100 with the smallest body registry and lifecycle needed by the proof.
- [ ] Existing ocean remains the verified production backend.
- [ ] One simple static or analytical lake **or** controlled river segment.
- [ ] WTR-230 for the activated body's authoritative parameters, revisions, events, late join, and fallback.
- [ ] Existing character submersion, projectile crossing, AI/audio consumers where applicable, and existing boat/buoyancy behaviour use `WaterQuery`.
- [ ] Weather input affects the activated visual water paths without changing gameplay authority.
- [ ] WTR-110, WTR-120, and WTR-130 continue only when they close verified current-ocean problems or fit approved budgets.
- [ ] WTR-200 is activated only when the chosen proof or an approved later feature genuinely requires a shallow-water decision.
- [ ] WTR-210 and WTR-220 remain conditional later expansion rather than Milestone 5 blockers.

Milestone 5 validates existing boat and buoyancy behaviour through `WaterQuery`; it does not require a dynamic boat-physics replacement, a shallow-water solver, full river networks, coastline breakers, waterfalls, flooding, or nested simulation domains.

**Exit criterion:** `existing ocean + canonical WaterQuery + one simple non-ocean body + existing submersion/projectile/boat consumers + weather + multiplayer reconstruction`.

## Milestone 6 — Terrain and Zeus transactions

- [ ] WORLD-001.
- [ ] TRN-GATE-1.
- [ ] TRN-010 through TRN-050.
- [ ] WORLD-004 is proven or an explicit specialised invalidation alternative is documented.
- [ ] SURF-000 produces a bounded decision or small vertical slice if surface-state work is justified.
- [ ] NAV-000 gathers evidence and preserves the safe invalid-region fallback; it does not block Preview 2 unless promoted.
- [ ] DEST-000 may define a first object-destruction slice but remains outside Preview 2 unless explicitly adopted.
- [ ] ZEU-000 through ZEU-020.
- [ ] ZEU-100 and ZEU-110.
- [ ] Preview 2 shipped.

## Milestone 7 — Far-world decision gate

- [ ] FAR-000.
- [ ] FAR-010 only if justified.
- [ ] FAR-020.
- [ ] No overlapping large far-tree, far-meadow, or far-terrain systems before the decision.
- [ ] Include the reference implementation if it becomes available before the decision.
- [ ] Use COORD-000 precision findings and STR-000 residency findings as formal inputs.

## Milestone 8 — Dynamic physics, animation bridge, and advanced smoke

- [ ] PHY-GATE-0 and PHY-005 produce the current backend decision for the profiles actually needed before broad dynamic-physics commitment. Activate `PHY_PROJECTILE`, `PHY_CHARACTER`, `PHY_DYNAMIC_BODY`, `PHY_VEHICLE`, `PHY_DESTRUCTION`, or `PHY_BUOYANCY` only when the corresponding scope is approved.
- [ ] PHY-006 reduces backend callbacks into authoritative semantic Poseidon events or state transitions.
- [ ] PHY-007 records and pins any adopted external dependency.
- [ ] PHY-020B where required by the selected backend and scope.
- [ ] Staged rigid bodies.
- [ ] Wheeled and tracked prototypes when justified by the backend decision.
- [ ] Boat physics.
- [ ] ANIM-000 may investigate or implement the bounded animation/physics bridge; it is not automatically blocking.
- [ ] Advanced smoke grids if justified.
- [ ] Obstacle-aware wind if justified.

## Milestone 9 — Broader asset compatibility

- [ ] PoseidonFormats extension.
- [ ] Offline compiler.
- [ ] Static Arma 2/OA compatibility.
- [ ] Animation and simulation semantics, informed by `ANIM-000` without requiring its suggested mechanisms.
- [ ] Broader Arma 3 compatibility.
- [ ] Mission/addon behaviour only after foundational formats are stable.
- [ ] SCR-000 observability evidence informs scheduler or semantic changes before a scripting rewrite is approved.

## Milestone 10 — Advanced authoring, water expansion, and experiments

- [ ] WTR-200, WTR-210, and WTR-220 where an approved post-Preview-2 release or authoring workflow justifies them.
- [ ] WTR-221 where justified.
- [ ] ZEU-120 advanced modules and cinematics.
- [ ] LIT-EXP where justified.
- [ ] Experimental physics or far-world approaches only through approved decision tickets.

A stronger execution overlay may activate an individual water-expansion ticket earlier when a concrete release slice depends on it, but broad water work must not displace Build Truth, renderer reconciliation, Compatibility Preview C0, or the editable-world slice without an approved priority decision.

# 17. Definition of done

A major feature is complete only when:

- [ ] Status ledger record exists and names the verified commit.
- [ ] Ledger contains class, obligation, execution mode, scheduling state, lifecycle status, milestone role, owner, and dependencies.
- [ ] Production code consumes it.
- [ ] It has no unexplained dead infrastructure.
- [ ] Debug visualisation or diagnostics exist.
- [ ] Deterministic test inputs exist where applicable.
- [ ] CPU, GPU, memory, upload, and bandwidth costs are recorded where applicable.
- [ ] Multiplayer authority and late join work, or N/A is justified and reviewed.
- [ ] Save/load and schema migration work when persistent.
- [ ] Missing optional content is safe.
- [ ] The feature can be disabled or reverted.
- [ ] Original data remains untouched.
- [ ] Quality profiles and unsupported-hardware fallback exist.
- [ ] Cross-platform status is recorded.
- [ ] An independent agent audits production call paths and dead infrastructure.
- [ ] Different visual quality settings do not change gameplay outcome.
- [ ] Third-party assets have verified source, licence, hashes, and provenance.
- [ ] Example assets in this roadmap were treated as suggestions, not mandatory choices.
- [ ] Better alternatives were considered and documented.
- [ ] The work contributes to a measurable release slice or an explicit gate.
- [ ] Ticket ID is unique and its ledger record passes schema validation.
- [ ] Implementation owner is not the final `SHIPPABLE` approver.
- [ ] Research tickets record an explicit outcome.
- [ ] Required preview outcomes have either the preferred implementation or an approved documented fallback.
- [ ] A Quick Win satisfies QW-000 and has not introduced undeclared architecture.


# 18. Final implementation philosophy

The target is one coherent world rather than disconnected effects:

- Weather drives clouds, light, wind, water, smoke, grass, and ballistics.
- Terrain deformation affects collision, vegetation, water, roads, and AI.
- Physics geometry supports collision, ballistics, smoke, and indoor lighting.
- Water combines the appropriate spectral, shallow-water, particle, and baked techniques behind one API.
- Zeus edits the same authoritative systems used by gameplay.
- Original content remains intact.
- Optional enhancements fail safely.
- Multiplayer consequences remain synchronised even when rendering quality differs.
- Temporal history, streaming, invalidation, precision, surface state, lighting, audio, scripting, frame pacing, animation, navigation, destruction, physics, and diagnostics should connect systems where that creates measurable coherence, but should remain specialised where forced unification would reduce quality or increase risk.
- Middleware choices remain evidence-backed implementation decisions rather than permanent identity commitments; adapters, conformance tests, and dependency records should make future improvement possible without encouraging needless churn.

The public identity is an independent modern evolution of Poseidon, not an Enfusion clone or a promise of complete later-generation compatibility. Compatibility claims must remain narrower than the evidence.

The master roadmap defines the constitution; the Execution Overlay runs the current project. The principal delivery risk is not lack of good ideas but activating too many of them simultaneously.

Agents should preserve strong existing systems, challenge weak assumptions with evidence, and avoid rewrites whose main benefit is architectural fashion rather than measurable improvement. The roadmap deliberately names promising directions without claiming that its authors have identified the optimal implementation. More capable agents are expected to improve the plan when they can demonstrate a better path, while respecting the project constraints, evidence requirements, compatibility goals, and release value defined here.
