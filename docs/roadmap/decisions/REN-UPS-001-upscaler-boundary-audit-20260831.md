# REN-UPS-001 — the generic upscaler boundary: audit, and the one gap that was real

Date: 2026-08-31
Branch: `agent-upscaler-boundary` (from `safety/asset-obs-001`)
Roadmap item: `docs/roadmap/POST-DLSS-MODERNISATION-ROADMAP.md` §1.1 "Generic upscaler boundary",
critical-path step 2.

## Summary

**The boundary already existed.** §1.1 opens "DLSS must not remain a special path spread
throughout renderer orchestration" — but REN-TEMP-001L Phase 13 had already built a
vendor-neutral `TemporalUpscaler` trait with an ordered backend chain. Written from the
roadmap text alone, this task would have re-implemented it.

What did **not** exist was the roadmap's *explicit capability description*. The chain
dispatch was clean, but three places in the orchestration recovered per-backend facts by
**string-matching the winning backend's `label()`**. That is precisely the
`if (dlss) … else if (fsr)` §1.1 says must go, and it is worse than an enum: a renamed
label changes renderer behaviour with no compile error anywhere.

That is what this change fixes, and nothing else.

## 1. Where the upscaler decision actually lives

There is no `if (dlss)` selector. The renderer owns an ordered `Vec<Box<dyn TemporalUpscaler>>`
and asks each backend in turn to fill the output-extent target; the first `Done` wins.

| Piece | Location |
| --- | --- |
| Trait, context, outcome | `engine/WgpuRenderer/rust/src/upscaler.rs:106-112` (`TemporalUpscaler`), `:65-94` (`UpscaleContext`), `:96-104` (`UpscaleOutcome`) |
| Chain construction | `engine/WgpuRenderer/rust/src/lib.rs:1647-1656` — `[DlssUpscaler (dlss builds + NGX route), FsrUpscaler, BilinearUpscaler]` |
| Chain execution | `engine/WgpuRenderer/rust/src/lib.rs:2292-2420` inside `render_post` |
| Readout to C++ | `lib.rs:2572` → `ffi.rs:1684` `active_upscaler` → `EngineWgpu.cpp:6395` → `Engine.hpp:1724` `TemporalInfo::activeUpscaler` |
| Panel consumption | `Dev/Debug/DebugOverlay.cpp:4635-4660`; `apps/cwr/Game/GameApplication.cpp:147` |

What each of the four paths consumes:

- **Native (id 0)** — bypasses reconstruction entirely. Not a backend: it is the *absence*
  of the chain. `ensure_hdr` (`lib.rs:2630-2657`) only allocates `hdr_upscaled` when
  `(width,height) != output || dlaa_active()`; with no upscale target the post chain reads
  the resolve directly and `last_upscaler` stays at its per-frame default (`lib.rs:3281`).
- **DLSS (id 1)** — `upscaler.rs:579-802`. Consumes colour texture, depth, velocity,
  exposure, optional history-control mask, jitter, reset, and the MV/jitter convention
  flags. Splits the frame around a raw Vulkan encoder (`upscaler.rs:738-785`).
- **FSR 1 (id 2)** — `upscaler.rs:244-368`. Consumes colour view, render/output extents and
  `fsr_sharpness` only. Ignores depth, motion vectors, jitter and history, by design.
- **Bilinear (id 3)** — `upscaler.rs:141-152`. Colour view and output view. Never fails;
  the guaranteed terminal fallback.

## 2. The twelve contract items

Graded per item as a whole: **(a) explicit parameter of the shared interface**,
**(b) present but implicit / through global renderer state**, **(c) absent**.

| # | Contract item | Grade | Evidence |
| --- | --- | --- | --- |
| 1 | internal render extent and output extent | **(a)** | `UpscaleContext.render` / `.output`, `upscaler.rs:79-80`; filled from `render_size()` (`lib.rs:2582`) and `config.width/height` at `lib.rs:2297-2298` |
| 2 | colour input, format, colour-space / pre-exposure convention | **(b)** | Input explicit (`color_tex`/`color_view`, `upscaler.rs:71-72`). Format is the crate-global `HDR_FORMAT` (`lib.rs:60`), not a context field. The HDR/linear convention is **hardcoded inside the backend** as `DLSS_FLAG_IS_HDR` (`dlss.rs:512`); FSR 1 independently assumes the same via its SRTM wrapper. Two backends assume; the contract never states. |
| 3 | depth input and explicit depth convention | **(b)** | Input explicit and optional (`upscaler.rs:73`). Reversed-Z is **hardcoded** as `DLSS_FLAG_DEPTH_INVERTED` (`dlss.rs:512`). A backend that got the convention wrong would fail silently. |
| 4 | motion vectors, units, direction and scale | **(b)** | Vectors explicit (`upscaler.rs:74`). Units/direction/scale arrive as *A/B debug arms* on settings — `mv_render_space`, `mv_flip` (`upscaler.rs:58-59`), resolved to `mv_scale` **inside** the DLSS backend (`upscaler.rs:724-730`). A per-backend tuning knob, not a stated renderer convention. |
| 5 | jitter offset and sequence phase | **(b)** | Offset explicit (`jitter_px`, `upscaler.rs:81`). Sequence phase is **not on the contract**: `frame_index` and the phase count are private to `Temporal` (`temporal.rs:140`, `:302-318`), configurable from C++ as `jitter_phases` (`ffi.rs:1653`) but never reaching `UpscaleContext`. `jitter_y_flip` is again an A/B arm. |
| 6 | exposure or pre-exposure | **(a)** | Exposure texture (`upscaler.rs:75`), manual `exposure_scale` scalar (`upscaler.rs:60`, sourced from the tonemap at `lib.rs:2341`), and `dlss_auto_exposure` (`upscaler.rs:56`). The most completely specified item of the twelve. |
| 7 | simulation tick identifier | **(c)** | No such concept anywhere in the renderer. Nothing in `lib.rs`, `ffi.rs`, `temporal.rs` or `wgpu_renderer.hpp` carries a sim tick. |
| 8 | snapshot generation | **(c)** | Absent. |
| 9 | presentation frame identifier | **(c)** | Absent from the contract. `Temporal::frame_index` (`temporal.rs:140`) is a private jitter counter, not a presentation id, and is never exposed. |
| 10 | history-reset token **and reason** | **(b) → (a)** | Token was explicit (`reset`, `upscaler.rs:82`). The reason existed — `Temporal::last_reset_reason()` (`temporal.rs:559`), with real values ("camera teleport", "render-size change", "world load") — but never crossed the boundary. **Made explicit by this change**: `UpscaleContext.reset_reason` (`upscaler.rs:89`). |
| 11 | optional reactive / transparency / history-control masks | **(a)** | `history_control` (`upscaler.rs:76`), `Option`, plumbed at `lib.rs:2319-2323`. The reactive weights (`reactive_sky`, `reactive_water`, `temporal.rs:166`) are baked into that mask upstream, which is the right place. |
| 12 | quality mode and optional sharpening policy | **(b)** | Quality mode is **not an input**: the DLSS backend derives it from the scale ratio (`upscaler.rs:619-628`) and returns it as an opaque `i32` in `Done { quality }`. Sharpening is split — `fsr_sharpness` is on settings, but the *policy* (`dlss_sharpen`, `ffi.rs` flags bit5) lived only in renderer state and was applied by matching the winner's label. The policy half is fixed here; the quality-mode half is not. |

**Tally: 3 explicit, 6 implicit, 3 absent** — becoming **4 explicit, 5 implicit, 3 absent**
after this change.

The three absent items (7, 8, 9) are a coherent group: they are the inputs a *frame
generation / reprojection* backend needs, and there is no such backend and no consumer for
them. Adding them now is exactly the speculative machinery this project has learned to
avoid.

## 3. Is there a capability description?

**Before this change: no.** `grep -rn "Capabilit\|fn caps" engine/WgpuRenderer/` found only
`WgrRuntimeCapability` (`include/wgpu_renderer.hpp:41`), which is a *renderer*-level feature
flag set, unrelated to upscaler backends.

The chain-with-`Skip` dispatch was better than enum branching, but the orchestration still
branched on the winner's **label string** in three places:

| `lib.rs` (pre-change) | What it recovered | Failure mode |
| --- | --- | --- |
| `if m.starts_with("DLSS ")` | "a temporal backend died, roll back the auto-enabled temporal path and 67% render scale" | A second temporal backend gets no rollback; users keep jittered 67% bilinear |
| `if let Some(("DLSS", q)) = winner` | "this backend has no internal sharpening, run external RCAS" | If FSR were ever renamed to match, its frame is sharpened twice |
| `match winner { Some(("DLSS",_)) => 1, Some(("FSR1",_)) => 2, … }` | the ABI id for the dev panel | A renamed label falls to `_ => 0` and the panel silently reports "native" while a backend ran |

All three are compile-silent. That is the defect, not a style preference.

## 4. What changed, and why it is the smallest useful change

`UpscalerCaps` (`upscaler.rs:33-47`) — three fields, each with an existing consumer:

- `info_id` — the backend owns its own ABI id, so id and implementation cannot drift.
- `needs_temporal_inputs` — the backend declares it requires jitter + motion vectors +
  depth. Drives the auto-defaults rollback.
- `wants_external_sharpen` — the backend declares it does no sharpening of its own.
  Drives the external RCAS pass.

`fn caps(&self)` on the trait; implemented by all three backends. The orchestration
(`lib.rs:2345-2440`) now carries the winner as its capabilities and asks the caps what to
do. **All three label-string branches are gone.** Plus `reset_reason` on the context
(contract item 10), which was already computed and simply never passed.

No new field was added without a consumer, and no backend, pass or shader was restructured.

**Why DLSS still works, argued from the code** (the game was not launched, per the task's
constraint): the change is behaviour-preserving by construction. `DlssUpscaler::caps()`
returns `needs_temporal_inputs: true, wants_external_sharpen: true, info_id: ID_DLSS`, and
DLSS is the only backend with either flag set — so each rewritten predicate selects exactly
the same frames as the string match it replaced:

- `temporal_backend_died` ⟺ the disabled backend was DLSS (only DLSS sets the flag) ⟺ the
  old `m.starts_with("DLSS ")`, since the message is `format!("{} disabled: …", label())`.
- `caps.wants_external_sharpen && self.dlss_sharpen` ⟺ old `winner == "DLSS" && dlss_sharpen`.
- `caps.info_id` ⟺ the old `match`, value for value (pinned by `info_ids_match_the_engine_abi`).

Nothing inside `DlssUpscaler::evaluate`, `dlss.rs` or `dlss_device.rs` was touched, and
`cargo test --features dlss` compiles and passes.

Four tests lock the contract (`upscaler.rs`, `mod tests`): the ABI id values, id uniqueness
and that no backend claims the native id, that spatial backends neither ask for external
sharpening nor require temporal inputs, and that the terminal fallback requires nothing.

## 5. Tests

| Suite | Before | After |
| --- | --- | --- |
| `PoseidonTests.exe` | 240,658 assertions / 3,008 cases, 0 failed | unchanged (no C++ touched) |
| `cargo test -p wgpu_renderer --profile rwdi` | 204 passed, 0 failed, 1 ignored | **208 passed**, 0 failed, 1 ignored |
| `cargo test -p wgpu_renderer --features dlss` | — | compiles; +1 DLSS caps test |

## 6. What remains

Honest list, in rough order of value:

1. **Contract items 2 and 3 — colour-space and depth convention.** Reversed-Z and HDR-linear
   are hardcoded in `dlss.rs:512` and independently re-assumed by FSR 1. They should be
   *stated by the renderer* on `UpscaleContext` and read by backends. Deferred because
   threading them into `create_feature` touches the live NGX path, and no launch was
   available to confirm DLSS output.
2. **Item 4 — motion-vector convention.** `mv_render_space` / `mv_flip` are A/B probe arms
   left in production. Once the owner's verdict on the correct arms is settled, they should
   collapse into one stated renderer convention.
3. **Item 12 — quality mode as an input.** Currently derived from the scale ratio inside the
   DLSS backend. A shared `QualityMode` on the contract would let the panel request DLAA
   explicitly instead of inferring it from `render_scale == 100%`.
4. **`dlaa_active()` (`lib.rs:2626`)** is a fourth DLSS-specific branch, in *target
   allocation* rather than chain dispatch — it decides that an upscale target is needed even
   at 1:1. A `wants_target_at_unit_scale` capability generalises it, but the query runs
   before the chain and would need per-backend eligibility. Left alone deliberately: the
   risk is breaking DLAA, and the payoff waits on a second temporal backend.
5. **`DebugOverlay.cpp:4660`** re-derives "is the sharpness slider live" from the id
   (`activeUpscaler == 2 || (activeUpscaler == 1 && dlssSharpen)`), duplicating policy the
   renderer now holds in caps. Surfacing one `sharpness_live` bit through `WgrTemporalInfo`
   would remove the duplication and the drift risk.
6. **Items 7, 8, 9** stay absent until a frame-generation or reprojection backend exists.

## 7. Verdict on critical-path step 2

**Substantially done, but not fully.** The structural demand of §1.1 — one renderer-facing
contract over Native / FSR 1 / DLSS, no vendor path spread through orchestration, an
explicit capability description rather than backend branching — is now met, and a future
XeSS or FSR 3 backend can be added as one more `impl TemporalUpscaler` with its own caps and
no orchestration edits. That was the stated goal ("without another renderer-wide refactor").

It should **not** be marked done unqualified. Five of the twelve contract items are still
implicit, and two of those (2 and 3, the colour-space and depth conventions) are the kind of
silent-failure surface §1.1 was written to eliminate — a new backend that guesses wrong gets
no error, just a wrong-looking frame. Items 7-9 are correctly absent, not missing.

Recommendation: mark step 2 done for the *boundary and capability* half, and carry items 1-3
of §6 above as a small follow-up scoped to conventions, to be done alongside the next backend
or the next DLSS convention verdict — whichever comes first.
