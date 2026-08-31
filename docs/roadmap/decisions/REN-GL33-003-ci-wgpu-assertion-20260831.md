# REN-GL33-003 — CI cannot start WGPU, so assert the refusal it can (2026-08-31)

Gate bullet 5 of `REN-GL33-001` — "CI contains an explicit WGPU-start assertion
and no GL33-only required job" — was scored the strongest blocker on the WGPU
sole-renderer gate. This document establishes what a hosted runner can and
cannot do, wires the strongest honest assertion that remains, demonstrates that
it fails when the contract is broken, and names precisely what the rest costs.

Read at `87d513ac` on branch `agent-ci-wgpu-assertion`. **No game binary was
launched for this document** — another session owns the single allowed game
instance. Everything asserted below about the *engine's* behaviour is either
read from source with a file:line, or was measured by the session that added
`WGR_FORCE_INIT_FAIL` earlier the same day and is cited as theirs. The failure
demonstration in §4 was run here, against stubs, and its limits are stated.

## 1. The constraint, established

### The runners

Every job in `.github/workflows/build.yml` and `lint.yml` runs on a
**GitHub-hosted** runner: `ubuntu-24.04`, `windows-2022`, `macos-26`. There is
no `self-hosted` label anywhere in `.github/`. None of these images has a GPU.

### Why "no GPU" is not the binding constraint

It would be easy to stop at "hosted runners have no GPU, so WGPU cannot start"
and reach for a software adapter. The renderer's own feature gate is stricter
than that, and it is what actually decides the question:

| Gate | Where | Consequence |
|---|---|---|
| `TEXTURE_BINDING_ARRAY` **and** `SAMPLED_TEXTURE_AND_STORAGE_BUFFER_ARRAY_NON_UNIFORM_INDEXING` | `engine/WgpuRenderer/rust/src/lib.rs:982-989` | `Renderer::new` returns `Err`; terrain needs descriptor indexing |
| `VERTEX_WRITABLE_STORAGE` | `engine/WgpuRenderer/rust/src/lib.rs:1015-1018` | `Renderer::new` returns `Err`; GPU grass writes instance data in compute and reads it in the vertex stage |

Adapter selection is `request_adapter` with `power_preference:
HighPerformance`, `compatible_surface: Some(&surface)` and **`force_fallback_adapter:
false`** (`lib.rs:953-958`), on an instance built by
`InstanceDescriptor::new_without_display_handle_from_env()` (`lib.rs:905`) —
which reads `WGPU_BACKEND` (wgpu-types 29.0.4, `src/backend.rs:153-161`). So the
code does *not* forbid a software adapter: `force_fallback_adapter: false` means
"do not *force* the fallback", not "reject one". A software device that
advertises the features above would be selected and accepted. **The gate is
capability, not vendor.**

### Which software adapters can clear it

- **wgpu's GL backend: never.** `TEXTURE_BINDING_ARRAY` does not appear anywhere
  in `wgpu-hal-29.0.4/src/gles/` — not once. The GLES adapter cannot expose it on
  any driver, software or hardware. (`VERTEX_WRITABLE_STORAGE` it *does* expose,
  `src/gles/adapter.rs:465`, which is what makes this easy to get half-right.)

  **This is a live finding, not a hypothetical.** `build.yml` set
  `WGPU_BACKEND: 'gl'` for the Trident job, with a comment explaining that GL is
  the backend that survives Xvfb. It could never have started the renderer. The
  six WGPU-pinned scenarios are all `ci-host-blocked` and skipped, so nothing
  depended on the value — but it was a trap laid for whoever unskips them, who
  would have debugged Xvfb for a refusal that was decided in the feature check.
  Changed to `vulkan` in this branch, with the reasoning in the comment.

- **Mesa lavapipe (software Vulkan ICD): credible, unmeasured.** wgpu's Vulkan
  backend maps `TEXTURE_BINDING_ARRAY` and its non-uniform-indexing companion
  onto `VK_EXT_descriptor_indexing` (`wgpu-hal-29.0.4/src/vulkan/adapter.rs:14-20,
  270-287`), and `VERTEX_WRITABLE_STORAGE` onto `vertexPipelineStoresAndAtomics`.
  lavapipe advertises both. Whether it survives the *rest* of bring-up — shader
  compilation, surface formats, the first frame, and at what speed — is not
  establishable from code and is exactly the sort of thing this project has been
  burned by asserting from source. It is now **measured on every push** (§3).

- **WARP (D3D12 software, Windows): credible, unmeasured, unwired.** wgpu-hal's
  dx12 instance enumerates through `IDXGIFactory1::EnumAdapters1` with no CPU-type
  filter (`src/auxil/dxgi/factory.rs:83-88`) and knows WARP by name
  (`src/dx12/adapter.rs:171`), so a WARP adapter would be offered if the image
  has one. WARP supports resource binding tier 3 and descriptor indexing. There
  is no Windows integration lane in CI at all today, so probing this means
  building one; §5 costs it.

### Verdict

**"Assert WGPU starts in CI" is not available today on any hosted runner, and
the reason is a feature gate rather than the absence of a GPU.** The one
GPU-free path that could clear that gate (lavapipe) is credible enough to
measure and too unproven to gate on.

## 2. Audit — was any of this already wired?

Seven roadmap items were found already-implemented in this repo on 2026-08-31,
so this was checked before anything was built. It was not:

- `grep -rn -i "pester\|boot_logs\|run_preview0\|Invoke-Pester" .github/` — **no
  hits.** `tests/smoke/boot_logs.tests.ps1` is invoked by no workflow, and
  `scripts/run_preview0_wgpu_check.ps1` only by hand from `RELEASING.md:30-34`.
- No workflow sets `WGR_FORCE_INIT_FAIL`; before this branch it appeared only in
  `GraphicsBackendWgpu.cpp` and two `docs/` files.
- `lint.yml`'s `preview0-ledger` job runs seven validators and one self-testing
  acceptance gate; none of them starts a binary.
- The `Preview0-WGPU` artifact (`build.yml:223-232`) ships `wgpu_renderer.dll`
  and a manifest built from file hashes and the CMake cache — the DLL is never
  loaded.

Bullet 5's description was accurate.

## 3. What was wired

### The assertion: the no-silent-fallback contract (blocking)

`scripts/check_wgpu_refusal.py`, run by the new **`WGPU refusal contract`** job
in `build.yml` against the built Linux client and the Demo data.

The contract it asserts is the half of bullet 1 that bullet 5 leaves untested:
*an explicit `--render wgpu` that cannot create WGPU must fail loudly and must
never fall through to GL33.* `WGR_FORCE_INIT_FAIL=1` triggers it at the factory
(`GraphicsBackendWgpu.cpp:29-33`) — **before any device or window bring-up**,
which is why it behaves identically on a runner and on a workstation with a GPU.

Three arms, and the first two exist so the third cannot pass vacuously:

| Arm | Command | Must hold |
|---|---|---|
| A, control | `--render dummy`, forced | exit **0**, no probe line, no refusal line |
| B, contract | `--render wgpu`, forced | exit **non-zero**; probe line present; refusal line present; **none** of `GL33: Initializing engine`, `Wgpu: creating renderer`, `defaulting to Auto` anywhere in the log |
| C, mechanism | (arm A vs arm B) | the probe line appears **only** in the forced wgpu arm |

Arm C is the one that matters most and is easiest to omit. A GPU-free runner
fails `--render wgpu` anyway, so arm B's non-zero exit on its own proves
nothing — it would stay green if `WGR_FORCE_INIT_FAIL` stopped being read
entirely. Requiring the probe line is what makes the forcing mechanism itself
the thing under test. Arm A is the guard against the other vacuity: a binary
that crashes on startup for an unrelated reason would otherwise "satisfy" arm B.

**What this does not assert: that WGPU starts.** Nothing in it touches a device.
The script says so in its own success output, and the job's name says
"refusal", not "start", so a reader of a green run cannot mistake one for the
other.

### The probe: does lavapipe work? (informational, `continue-on-error`)

The new **`WGPU start probe (lavapipe, informational)`** job installs
`mesa-vulkan-drivers`, records `vulkaninfo --summary` and the descriptor-indexing
lines as a second witness, then runs `--render wgpu --check` under Xvfb with
`VK_ICD_FILENAMES` pinned to the lavapipe ICD, and prints its own verdict:

```
PROBE RESULT: WGPU STARTED on lavapipe. Gate bullet 5's start half is now reachable in CI.
PROBE RESULT: WGPU did not start on lavapipe (exit=N). Bullet 5 remains hardware-blocked.
```

It cannot fail the build. Its value is that `REN-GL33-001`'s open question R2 —
"does WGPU start under Xvfb on the hosted CI image?" — stops being a guess that
has to be re-argued in every document and becomes a line in every run's log,
with the adapter capabilities beside it. **If it goes green and stays green,
promote it**: drop `continue-on-error`, and bullet 5's first half is genuinely
satisfiable without buying hardware. That is a one-line change made on evidence
rather than on hope.

### Not wired, deliberately

`tests/smoke/boot_logs.tests.ps1` is candidate 3 in the brief and stays
unwired. Its game-startup tests run `--render dummy`, which never reaches the
WGPU factory, and its two rendering tests are `--render gl33`. Wiring it would
close a real gap (the file is run by nothing) but it would add **no** WGPU
coverage, and calling it progress on bullet 5 would be the kind of
source-shaped reading that put bullet 7 wrong. Recommended as its own small
task, not counted here. Its stale comment claiming the bare `--render` default
is `gl33` — it has been `wgpu` since 2026-08-06 — is corrected in this branch.

## 4. Showing it can fail

`scripts/check_wgpu_refusal_selftest.py` drives the checker against eight stub
clients — one that honours the contract and seven that break it in each way it
can actually break — and requires exactly one pass and seven failures. It is run
by `lint.yml`, in the same shape as that job's existing self-test of the
capture-comparison acceptance gate, and needs no GPU, no game binary and no game
data.

Run here, 2026-08-31, all eight as specified:

```
[ok ] good               -> PASS (want PASS) : honours the contract
[ok ] silent-fallback    -> FAIL (want FAIL) : explicit wgpu falls through to Auto and starts GL33
       FAIL: explicit --render wgpu could not create WGPU and still exited 0.
[ok ] fallback-then-fail -> FAIL (want FAIL) : falls back to GL33, then exits non-zero for another reason
       FAIL: explicit --render wgpu refused, but GL33 was initialized anyway
             ('GL33: Initializing engine' in the log).
[ok ] exit-zero          -> FAIL (want FAIL) : refuses, logs it, but exits 0
[ok ] refusal-unlogged   -> FAIL (want FAIL) : refuses and exits 1 but logs no reason (the RptF regression)
       FAIL: forced arm never logged the refusal line 'refusing automatic GL33 fallback'.
[ok ] no-probe-log       -> FAIL (want FAIL) : forcing works but the probe line is gone
[ok ] probe-ignored      -> FAIL (want FAIL) : WGR_FORCE_INIT_FAIL no longer read (arm B passes vacuously)
       FAIL: forced arm never logged the probe line ... without it this arm's non-zero exit
             proves nothing, because a GPU-less runner fails WGPU anyway.
[ok ] control-dies       -> FAIL (want FAIL) : the control arm itself is broken, so nothing is proven
```

Two of these deserve naming, because they are the failures a weaker checker
would have shipped green:

- **`fallback-then-fail`** — falls back to GL33 and *then* exits non-zero for an
  unrelated reason. The exit-code rule cannot see it; only the creation markers
  can. A checker that only tested the exit code would call a silent fallback a
  pass.
- **`probe-ignored`** — `WGR_FORCE_INIT_FAIL` is no longer read at all. Every
  other signal is unchanged, because the runner has no GPU and refuses anyway.
  This is the specific way this job would rot into a green tick, and arm C is
  the only thing that catches it.

**The honest limit:** these stubs test the *checker*, not the engine. That the
engine still emits those lines and that exit code is what the `wgpu-refusal`
job establishes on every push, against the real binary. It has not been run
against the real binary in this session — no game was launched here — so the
first CI run on this branch is the first real execution, and until it is green
the wiring is unproven end-to-end. Say so rather than claiming the job passes.

## 5. The second half of bullet 5: "no GL33-only required job"

`build.yml:29` pins `TRIDENT_RENDER: gl33`, consumed at the Trident invocation;
`--skip-tag ci-host-blocked` skips all six WGPU-pinned scenarios. 111 of 117
scenarios run on GL33 and none on WGPU. Nothing here changes that, and the
options for changing it are, in ascending cost:

1. **lavapipe on the existing hosted runner — £0, already measured.** If the
   probe job goes green, the cheapest real step is not flipping
   `TRIDENT_RENDER` wholesale: it is unskipping the *one* scenario written for
   this (`shadow_map_scene_dump`) as its own non-blocking job, then promoting it.
   The known risk is **speed**, not capability: the suite already takes ~1554 s
   at `-j2` on llvmpipe GL, the cost is dominated by game startup under Xvfb, and
   lavapipe rasterises a full 3D frame on the CPU. A green probe means "WGPU can
   start", not "111 scenarios fit in 35 minutes". Expect to move a subset, not
   the suite.
2. **A Windows WARP probe — a few hours, no hardware.** There is no Windows
   integration lane today, so this means a new job that downloads the Demo data
   and the existing `Preview0-WGPU` artifact and runs `--render wgpu --check` on
   `windows-2022`. Worth doing as a second `continue-on-error` probe: WARP's
   feature set is a better fit for the gate than lavapipe's, and if it starts, it
   is a *second* GPU-free path and Windows is the tier-1 platform. Unmeasured
   here; the enumeration reading in §1 is the only evidence.
3. **GPU-backed hosted runners — recurring cost, owner decision.** GitHub offers
   GPU larger-runners on paid plans, Linux only. Zero maintenance, per-minute
   billing, and the Trident suite is long — the bill scales with exactly the
   thing that is already slow.
4. **A self-hosted runner with a GPU — the owner's machine, and a security
   decision.** This is the only option that gives release-tier Vulkan validation
   and full-speed integration runs. The cost is not the hardware; it is that a
   self-hosted runner attached to a repository accepting pull requests **runs
   untrusted code from forks on that machine**, so it needs the standard
   mitigations (restrict to non-fork triggers, ephemeral containers, or a
   separate runner group). Naming that is the point; choosing it is not this
   document's call.

**Recommendation:** do not schedule 2 through 4 yet. Read the probe first. It
costs nothing per run and it decides between "free" and "buy something", which
is the only question here that is worth money.

## What this does not establish

- **That WGPU starts anywhere.** Not on a runner, not on lavapipe, not on WARP.
  §1 establishes what would have to be true; nothing here shows it is.
- **That the new `wgpu-refusal` job passes.** No game binary was launched in
  this session. The checker's logic is demonstrated against stubs; its first run
  against the real client is the first CI run on this branch.
- **That lavapipe would be fast enough to be useful** even if it starts. §5.1
  gives the reason to doubt it; nobody has timed it.
- **That `WGPU_BACKEND: 'vulkan'` fixes the six skipped scenarios.** They remain
  `ci-host-blocked` and skipped. The change removes a value that was provably
  incapable of working; it does not make the scenarios run.
