# WLD-013 — Mount a bank where the archive says it lives

- **Ticket:** WLD-013
- **Date:** 2026-08-10
- **Resolves:** the open `known_failure` recorded under WLD-011
- **Scope:** bank mount prefix only. No world loading, no rendering of A3 content.

## The problem

`GameState.cpp` mounts addons with `LoadBanksEx("addons", true, true)`, which sets
`emptyPrefix`, so `BankList::Load` derived a bank's virtual root from its **filename**.
The archive's own declared root — the `prefix` property in its header-extension
block — was read into `_properties` but never consulted.

For OFP-era archives the two coincide. For Arma 3 they do not: `rocks_f.pbo`
declares `a3\rocks_f`, and every path a world references begins with that. Mounted
under `rocks_f`, not one of them resolves.

## Why this was safe to change

Surveyed across the local installs before touching anything:

| install | archives | with header-extension | declare a prefix | prefix ≠ filename |
| --- | ---: | ---: | ---: | ---: |
| CWA retail | 215 | 4 | 3 | **0** |
| CWA Demo | 14 | 1 | 1 | **0** |
| Arma 3 | 487 | 487 | 487 | **483** |

Preferring the declaration is a **no-op on every local OFP/CWA archive** — none of
the three that declare a prefix declares one that differs from its filename — and
corrects 483 of 487 Arma 3 archives.

`anims_f_data.pbo` is the case that settles it on principle rather than counts: it
declares `a3\anims_f\data`, a multi-segment root that no rule based on the
filename could produce under any separator convention.

## What changed

`ResolveBankMountName(declaredPrefix, bankPrefix, bName, emptyPrefix)` — a free
function in `QBStream.hpp`, so the rule is testable without a filesystem or an
open bank. It prefers the archive's declaration and falls back to the previous
caller-derived name when there is none.

The declaration is rejected, falling back, if it contains `..` or `:` or exceeds
240 characters. It comes out of a file and becomes the prefix that lookups resolve
against, and `SetPrefix` copies into a fixed 256-byte buffer.

## Verification

- `[Banks]` unit cases cover the declared-wins, fallback, and traversal-refusal
  branches. `PoseidonCoreTests` full run: 792 passed, 4 skipped, 6,085 assertions.
- `run_preview0_wgpu_check.ps1 -Backend wgpu` passes: the game boots, selects the
  WGPU backend, and mounts its banks.
- `-MissionCaptureSmoke` passes and the capture was **opened and inspected**, not
  merely reported: terrain, buildings, foliage, the soldier, grass, sky and
  shadows all render, visually identical to the pre-change capture.

## What this does NOT claim

This makes A3 archives *addressable*. It does not load an A3 world, instantiate an
A3 object, or render A3 content — none of that path exists yet. No A3 addon has
been mounted in a running game; the corpus survey plus the unmodified OFP smoke is
the whole of the evidence.

`BankList::Unload` still derives its search key the old way and so cannot find a
bank mounted under a declared prefix. Left as is and commented, because nothing
calls it.

The `AutoBank` de-duplication check in `GameState::LoadBanks` still tests the
filename-derived key, before the archive is opened. For an archive that declares a
different root, that check now tests a key other than the one the bank mounts
under. Untested for A3 content and noted here rather than silently changed.

## Next blocker

Bridging `Oprw25World` into the engine's `Landscape`/`World`. That is what a
screenshot of an Arma 3 map depends on, and none of it exists yet.
