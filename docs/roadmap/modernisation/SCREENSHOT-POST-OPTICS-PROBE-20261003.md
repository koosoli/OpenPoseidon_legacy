# Same-frame screenshot optics receipt

## Decision and scope

The actual sunny-snow captures in `sunny-normal-exposure-trace-20261003-202703-3e0c79` have settled two-camera frames and matching camera hashes, but the existing exposure log reports a previous completed scale. It cannot establish which HDR radiance, meter or scale produced a particular washed-out PNG. Add an opt-in screenshot-only float receipt before considering any exposure, snow colour or sky adjustment. This diagnostic changes no lighting, material, exposure or tonemap equation.

Owned files: renderer `lib.rs`, `tonemap.rs`, `tonemap.wgsl`, `exposure.rs`, new private `post_optics_probe.rs`, and this document. No C++/FFI/public ABI, new material resources, normal-frame HDR readback, or game/deployment action.

## Exact live resource and frame contract

`WGR_SCREENSHOT_POST_OPTICS=1` is an exact-one opt-in cached once. OFF creates no probe state/pipeline, retains no extra view handles, and performs no diagnostic draws/copies/maps. ON still encodes a probe only for an actual requested screenshot and only after Exposure has encoded its current reductions, adaptation and scratch-to-current publication in the same command encoder. The probe precedes the existing Tonemap draw.

The fragment entry uses Tonemap's existing group0 bind verbatim: its actual final postprocessing HDR view (including active resolve/upscale/DOF/underwater selection), nearest HDR sampler, bloom view and linear sampler, Tone uniform buffer and current exposure texture. It shares the same WGSL module and exact compilation override array as production. Group1 borrows Exposure's actual last 1×1 meter and actual Exposure uniform buffer. Diagnostic source identity compares real `wgpu::TextureView` handles: the independently rebound meter source and scale must match Tone's actual bound HDR and scale. Matching dimensions alone cannot admit a stale resource. Existing HDR/meter texture usages are unchanged.

Internal checked monotonic request and render-attempt serials, plus Tone bind-publication serial, are encoded by the GPU as four exact 16-bit limbs apiece. No lossy u64-to-f32 conversion or NaN payload is used. An overlapping pending screenshot request, exhausted serial, duplicate resolve, unavailable meter/bind, unsupported night vision, missing/invalid main camera, zero/out-of-range dimensions, nonfinite float, stale GPU identity, or partial metadata row refuses the receipt. Failed/skipped render attempts clear prior pending packets. None of these refusals changes legacy screenshot behaviour.

Original and prepared main-camera projection/view/position bit hashes are frozen for the actual render attempt after camera preparation and before surface acquisition. FNV-1a uses little-endian f32 bits in projection, view, camera-position order, including all four position components; prepared jitter can therefore differ visibly from the original hash. These hashes are provenance, not a claim that a particular tile depicts snow or a specific material.

The 25×28 RGBA32F target is 11,200 bytes; aligned staging is exactly **14,336 bytes**, below the compile-time 16KiB cap. Four 5×5 output-pixel tiles each produce four rows: raw HDR, raw bloom, precurve RGB after actual bloom/manual/current scale/white balance, and production Hable or passthrough curve output before desaturation/display grading. These are scene-linear diagnostics, not the saved display PNG.

Metadata rows (first column, repeated identically across the row):

| Row | GPU values |
|---|---|
|16|weighted log2-luminance, meter weight, current scale, compiled linear white|
|17|manual exposure, mode, encode, temperature|
|18|tint, contrast, saturation, lift|
|19|gain, bloom intensity, threshold, knee|
|20|night-vision strength, gain, noise, vignette|
|21|compiled desaturation start/strength, actual bound HDR dimensions|
|22–24|exact request, render-attempt and source-publication limbs|
|25|actual Exposure enabled, key, min scale, max scale|
|26|actual Exposure rate, sky weight, padding|
|27|actual bloom dimensions, output dimensions|

All selected pixels and metadata must be finite. After the existing screenshot RGBA readback succeeds, the paired tiny float readback is validated and logged with exact decimal request/frame/source tokens on every line. `PostOptics completed pairedScreenshot=1` means same-submission renderer screenshot RGBA completion, **not** an independently verified saved PNG path/hash. The installed harness must join the next previously unseen token to its screenshot completion and saved PNG hash. A failed screenshot readback drops the float packet.

## Use and validation

Default tile centres are output fractions (1/2,2/5), (1/2,13/20), (1/4,1/2), (3/4,1/2). Optional `WGR_SCREENSHOT_POST_OPTICS_TILES=x,y;x,y;x,y;x,y` supplies exactly four integer centres; all 5×5 tiles must remain inside the output. This avoids whole-frame readback and lets the actual church fixtures target snow, sea, shaded terrain and sky at matching camera poses. Tile semantics require actual image inspection.

Lightweight cached-library compilation uses the production Tone/Exposure/probe modules and exact FFI Tone/Exposure structs. Six CPU/parser tests pass: WGSL entry validation, strict bounds/byte cap, exact large identities, truncated/nonfinite/stale/NV rejection, overlapping/exhausted serials, and original/prepared camera bits/refusals. The normal `lib.rs` parses, and `git diff --check` passes. Full renderer integration and actual device/runtime are still parent-owned and pending.

Parent device command, with the normal repository Cargo target/runtime environment:

```powershell
$env:WGR_SCREENSHOT_POST_OPTICS='1'
cargo test --manifest-path engine/WgpuRenderer/rust/Cargo.toml post_optics_probe:: -- --include-ignored --nocapture --test-threads=1
```

The ignored actual-device fixture supplies known constant HDR and bloom through the production Tone bind, encodes the real Exposure reduction/adaptation before the diagnostic, checks actual meter/scale, all four tiles' raw and precurve/curve values, compiled white and actual Exposure uniforms, and refuses both independently changed HDR source and independently replaced scale. No actual-device success is claimed by the source candidate.

Installed runtime must confirm ON receipt paired with the exact saved sunny church PNG (near/far matching poses), actual HDR/source/output dimensions and camera provenance, and deliberate invalid tile/NV refusals. OFF must produce ordinary screenshots without `PostOptics` packets. Quiet performance runs keep the flag OFF. This evidence may support a later bounded correction; this diagnostic is not visual acceptance or an exposure fix.
