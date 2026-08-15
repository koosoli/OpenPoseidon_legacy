# RND-039 — Eye adaptation already exists, is deliberately off, and its metering is tuned to do the opposite of what was asked

**Date:** 2026-08-15
**Branch:** `feat/texture-streaming` (worktree `.claude/worktrees/texstream`)
**Status:** **Read-only finding. No code changed.** The request is a *tuning and metering*
decision, not a feature to build.
**Related:** the owner's feature request 3c in `HANDOVER-PROMPT.md`

## The request

> "when the player looks in the direction of the sun, the iris adjusts and it gets a bit darker,
> because now the sky is often just almost white out when looking at the sun."

The handover framed this as a feature and asked whether the engine has any adaptation term at
all, or tone-maps at a fixed exposure. **It has one, and it is complete.**

## What actually exists

A full auto-exposure chain, not a stub:

- `engine/WgpuRenderer/rust/src/exposure.wgsl` — a luminance pyramid reduces the linear HDR
  scene to a 1×1 weighted average log-luminance (`fs_lum_first` → `fs_lum_down`), then
  `fs_adapt` eases a persistent exposure *scale* toward `key / avgLuminance`, clamped to
  `[min_scale, max_scale]`. The tonemap multiplies by that scale, so no exposure parameter
  lives in the tonemap itself.
- `engine/WgpuRenderer/rust/src/exposure.rs` — the pipelines, the ping-pong history target.
- `Engine.hpp:1756-1770` — `ExposureSettings`, surfaced in the ImGui Tonemap tab.

## Why looking at the sun does not darken

Two independent reasons, both deliberate, both documented in the code.

**1. It is off by default.** `Engine.hpp:1760`:

```cpp
    // Manual filmic exposure is the stable default. Auto exposure remains available
    // in the Tonemap tab, but its 4x adaptation range can flash the whole scene
    // when a player turns from dark terrain toward a bright sky or water glint.
    bool enabled = false;
```

That comment describes **exactly** the failure mode the handover independently warned about —
"the risk to avoid is adaptation that pumps visibly as the camera turns, which is worse than a
flat exposure". Someone already tried this and backed it out. Turning `enabled` on and shipping
it would rediscover their result.

**2. Even switched on, the metering is built to ignore the sky.** `Engine.hpp:1770` sets
`skyWeight = 0.3f`, and `exposure.wgsl:50` spends it:

```wgsl
fn meter_weight(uv: vec2<f32>) -> f32 {
    return mix(ep.sky_weight, 1.0, smoothstep(0.2, 0.8, uv.y));
}
```

The top of the frame is weighted at 0.3 against the ground's 1.0, expressly "so a bright sky in
view doesn't drag the average up and over-darken the ground". A sun high in frame therefore
moves the metered average by less than a third of its true contribution. **The one thing the
owner wants to trigger adaptation is the one thing the metering is tuned to discount.**

So this is not a missing feature. It is two settings pointing away from the request, each for a
reason that was good when it was written.

## Why the obvious fix is wrong

Raising `skyWeight` toward 1.0 and setting `enabled = true` would darken when looking at the
sun. It would also:

- darken whenever the camera merely tilts up at an empty bright sky, which is not what was
  asked and is the "flash the whole scene" case the existing comment names;
- brighten again on the way down, so ordinary look-around pumps the whole frame;
- act on a **screen-space** heuristic (`uv.y`) that knows nothing about where the sun is — it
  cannot distinguish "looking at the sun" from "looking up".

The request is directional, and the current metering has no direction in it.

## What would actually satisfy it

Three changes, and the first is the one that carries the idea:

1. **Meter on the sun, not on the top of the frame.** The sun direction is already available to
   the renderer (the godray and sky paths consume it). Weight the metering by the angle between
   the view vector and the sun direction, so the term rises when the sun is *in front of the
   camera* and stays flat when the camera merely tilts up. This replaces a screen-space proxy
   with the actual quantity, and it is the difference between "iris adjusts toward the sun" and
   "screen dims when you look up".
2. **Asymmetric rate.** A real iris closes fast and opens slowly. One `rate` (0.03/frame, and
   note it is framerate-dependent — see below) used in both directions is what makes adaptation
   read as pumping. Fast-darken / slow-brighten is the standard fix and it directly targets the
   objection in the `enabled = false` comment.
3. **A much narrower range than 4×.** `minScale 0.25 / maxScale 4.0` is a 16:1 span. The
   request is "a bit darker", so something like 0.7..1.15 gives the effect without any
   possibility of the whole-scene flash. Most of the historical risk here is range, not concept.

**A defect to fix on the way past, independent of all of the above:** `rate = 0.03f` is
documented as "per-frame adaptation ease (0..1; **framerate-dependent**)". Adaptation speed
therefore changes with framerate — on worlds measured this session that is 20 fps to 45 fps, so
the same setting adapts more than twice as fast on one world as another. It should be converted
to a time constant (`1 - exp(-dt / tau)`). Any tuning done before that fix is tuning against
the framerate of whichever world it was tuned on.

## What this does not claim

- **Not measured, not captured.** This is a code read. Nobody has turned `enabled` on and looked
  at the result, so "it pumps" is the previous author's claim plus the handover's independent
  warning, not an observation from this session.
- There is **no env lever** to enable auto-exposure — `WGR_EXPOSURE` sets a *manual* exposure and
  switches auto-tonemap off, which is a different thing. Testing this today requires the ImGui
  Tonemap tab, i.e. a human at the keyboard. Adding `WGR_AUTO_EXPOSURE` would make it
  A/B-able from a capture harness, and that is the cheapest next step.
- Priority: this is a **feature request**, and it is behind the shadow, cockpit, road and
  streaming regressions. It is recorded now because the finding — "it exists and is off on
  purpose" — is what stops the next session from building it from scratch.
