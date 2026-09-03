# REN-DEV-004 — Everything shipped on by default today is switchable in the dev panel

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Reason:** the owner's instruction — an env var is not a switch they can reach.

## What changed

**GI probe basis** (REN-GI-009) — a combo in the GI tab: *Ambient cube (6 faces)* / *SH-1 (worse
— see REN-GI-008)* / *SH-2 (default)*. It replaces `WGR_GI_BASIS` as the primary control; the env
var still sets the startup value. The FFI's reserved `WgrGi::debug` word, which no shader ever
read, carries the basis, so this cost no ABI change.

**SH-2 is the default now.** REN-GI-009 measured it better on every shaded surface and identical
on every lit one for about 0.2 ms; the owner approved the flip before testing it, and the panel
combo plus `WGR_GI_BASIS=cube` are the way back.

**Cheap clouds in the water reflection** (REN-SKY-004) — a checkbox beside the other cloud
controls. This was the one feature shipped *on* today whose only control was an env var. It rides
the sky **look** struct's `night_params.w`, which was a genuine pad: `WgrSky::night_params.w` is
the fog range, but the LOOK struct's copy was never read (lib.rs copies lanes 0..2 only). So no
ABI bump for this either. The flag never reaches a shader — it picks which cloud *pipeline* the
planar pass records, so the renderer holds it as a plain field.

## Verified on the deployed binary

`Wgpu GI probe volume: … basis SH-2` in the log, and `Planar: clouds 1.09 ms` on the open-sea
pose — the 32-step arm measured 1.20 and the 128-step one 2.55, so both defaults are live.

## What is deliberately still env-only

Three knobs, and none of them changes anything unless you set it:

| knob | default | what it is |
|---|---|---|
| `WGR_GI_RSM_ISO` | off | REN-GI-007's ablation: spreads the sun bounce isotropically. A diagnostic that produced a finding, not a setting. |
| `WGR_WATER_FFT_CASCADES` | the preset's own count | REN-SKY-004's secondary measurement: caps live spectrum cascades. |
| `POSEIDON_MARK_TTL` | 10 s, the shipped value | PERF-026's ablation: footprint lifetime, 0 spawns none. |

The rule this follows: **anything on by default gets a panel control; an instrument that is off by
default stays an env var.** Putting measurement ablations in the panel would invite turning the
game's own features off by accident, and they are not features.
