# CORE-NEG-003 — The ABI guard existed, was tested, and checked four structs out of forty-five

- **Ticket:** CORE-NEG-003
- **Date:** 2026-08-14
- **Branch:** `grass-lod-dissolve-photo-cards`
- **Scope:** the C++/Rust FFI handshake only. No renderer behaviour changes.
- **Amends:** CORE-NEG-002 (*Versioned C++/Rust ABI*), whose runtime claim this
  incident disproved.
- **Surfaced by:** the moon investigation of 2026-08-13, which spent a session
  diagnosing a rendering bug that was not in the source.

## The claim

`WgrSkyRuntime` grew from 80 to 112 bytes without an ABI version bump, and the
handshake could not tell. An engine binary built before that change shook hands
successfully with a renderer built after it, then handed over an 80-byte struct
the renderer read 112 bytes from. There was no error, no entry-point failure and
no log line — just wrong pixels.

CORE-NEG-002 recorded the opposite as validated: *"C++ refuses an ABI mismatch
before renderer creation"*, with three test ids including
`ffi-separately-built-dll-refusal`. **The guard was real, the tests passed, and
neither covered the thing that drifted.**

## What actually happened

`f88f0ea` added the moon disc, which added `moon_params` and `moon_sun` to
`WgrSkyRuntime`. The diff does exactly what the contract asks — it updates the
size assertion:

```
-static_assert(sizeof(WgrSkyRuntime) == 80, ...)
+static_assert(sizeof(WgrSkyRuntime) == 112, ...)
```

That assertion protects a *single* build. It fires when the two sides are compiled
together and disagree. It says nothing about a C++ binary and a Rust cdylib
compiled at different commits and deployed side by side — which is the only
configuration this project actually ships, and the one `Install.ps1` exists to get
right.

`WGR_ABI_VERSION` was not bumped. `WgrAbiCheck` (`wgpu_renderer.hpp:795-806`,
validated at `rust/src/ffi.rs:1360-1369`) compared `sizeof` for exactly four
types: `WgrAbiCheck`, `WgrSurfaceDesc`, `WgrLogCallbacks` and `WgrFrame`. None of
them changed. The handshake passed.

The owner's retail install held `PoseidonGame.exe` dated 12 Aug 03:06 — a day
older than `f88f0ea` — beside a 13 Aug `wgpu_renderer.dll`.

## Why the symptom was so convincing

The moon fields came from whatever followed the struct on the stack, and what
followed it was the moon *direction*. The per-frame diagnostic reported:

```
radius=-0.57960  lit=-0.431  discScale=0.691504   dir=(-0.5796, -0.4311, 0.6915)
```

`radius`, `lit` and `discScale` are exactly `-dir.x`, `dir.y`, `dir.z`.

Read without that arithmetic, the numbers look like a rendering bug with a
coherent story: a negative disc scale failing the `moonDiscScale > 0` draw gate at
`EngineWgpu.cpp:5270`, a lunar radius of 49 degrees against a true 0.26, a lit
fraction of −0.686, several adjacent fields wrong at once. That last property is
the actual tell — it is the signature of a layout mismatch, not of a sign error —
but it was read as evidence of a *deeper* bug rather than a different one. A
capture was taken, the disc was absent, and a defect was reported that did not
exist in the source.

`cloudCover=0.50` was mistaken for corroborating evidence. It is the
weather-driven value (`EngineWgpu.cpp:5081-5085`) and reads 0.50 on a correct
build too.

## What changed

`WgrLayoutHash()` at the bottom of `wgpu_renderer.hpp`: a `constexpr` FNV-1a over
`sizeof` of all 45 structs that cross the boundary, carried in a new
`WgrAbiCheck::layout_hash`. `sizeof(WgrAbiCheck)` assertion 24 → 28.
`WGR_ABI_VERSION` 4 → 5. `wgr_abi_validate` now refuses on hash mismatch and logs
both hashes, the renderer build id, and *"deploy both, not one"*.

`wgr_abi_layout_hash()` is exported from Rust as a diagnostic but deliberately
**not** declared in the header. Importing it would make an older DLL fail at
Windows module load, before the handshake could produce its diagnostic — the same
reasoning already recorded for `GetRuntimeCapabilityFlags` at
`EngineWgpu.cpp:4536`. It is fetched with `GetProcAddress` instead.

Two Rust tests: `matching_abi_declaration_is_accepted`, and
`drifted_shared_struct_layout_is_refused`, which simulates this exact 80-byte
drift.

## What this does not cover

A **same-size field reorder** still passes. The hash is over sizes, not offsets or
names, so swapping two `vec4` members inside one struct is invisible to it. For
the sky UBO that gap stays covered by `sky/mod.rs`'s field-order test; elsewhere
it is uncovered, and a full offset-and-name hash is the honest next step.

It also does not cover semantic drift — a field that keeps its size and changes
meaning.

## The generalisable part

Three things are worth carrying forward, and the third is the expensive one.

1. **A compile-time assertion is not a deployment guard.** Every struct here
   carried a correct `static_assert` throughout, and the failure happened anyway,
   because the two binaries were never compiled together. Any check that only
   holds within one build says nothing about the pair that ships.

2. **A guard that names its members by hand will drift from the set it protects.**
   Four types out of forty-five were listed, and the list was never revisited. The
   fix is not "add `WgrSkyRuntime` to the list" — it is to derive the check from
   the whole set so that adding a struct cannot silently skip it. `ffi-contract.md`
   now states the append-to-both-lists rule, but the hash is what enforces it.

3. **A capture is only evidence if the binaries under it are a matched pair.**
   The session that found this was following a good rule — verify visual features
   by screenshot, not by reading code — and the rule produced a false positive
   anyway, because the capture ran the game out of the retail install rather than
   through `Start.ps1`. CLAUDE.md warns that deploying *one* binary gives an
   instant `Entry Point Not Found`; it does not warn about the quieter case, both
   present and mismatched. **Launch through `scripts/Start.ps1`**, which always
   runs the matched pair out of `dist\`, and check both timestamps before believing
   a measurement.

## Correction owed to CORE-NEG-002

That ticket stays VALIDATED at its own verification commit — its guard did work
for the four types it named, and its owner smoke test did exercise the boundary.
But its `fallback` line, *"C++ refuses an ABI mismatch before renderer
creation"*, was not true in general at the time it was written, and a reader would
have acted on it. A `known_failures` entry now records that, pointing here.
