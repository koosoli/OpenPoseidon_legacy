# When a surface renders "transparent", prove its fragments EXIST before touching shading

Written after MAT-052 (2026-08-26): the "transparent cockpit" survived two
weeks, two sessions, and SIX dead theories because every probe assumed the
geometry was being drawn and only its shading was wrong. It was never drawn:
wgpu had no `UpdateProjection` override, so the cockpit's
`SetClipRange(0.01, 50)` bracket never reached the GPU and everything nearer
than the world near plane was clipped. Fixed in `7e9693c4`.

## The rule

A "transparent" or "wrong-looking" surface has exactly three possible states,
and they need OPPOSITE fixes:

1. **Not drawn at all** (clipped, culled, skipped, never submitted) — you see
   the background, possibly modified by whatever IS drawn there (glass tint,
   cloud deck). Shading doors all A/B to "no change" — which looks like six
   dead theories but is ONE fact.
2. **Drawn but composited wrong** (blend, coverage, pass order).
3. **Drawn and composited right but shaded wrong** (lighting, material fold).

Every minute spent on 2/3 before excluding 1 can be wasted. MAT-052 burned
five same-binary A/Bs (fallback texel, glass sheen, emissive, alpha policy,
backface culling) on a surface that did not exist.

## The ladder — run it in this order

1. **`WGR_KILL_TEX=<comma-separated substrings>`** (DrawSectionTL): DROP every
   draw whose texture path matches. If the suspect pixel CHANGES, that path
   drew it — go to compositing/shading. If it does NOT change, that path never
   drew it — go to submission/clipping. Binary-search a texture list against
   captures; a surface identifies itself in ~4 runs.
2. **`WGR_SECTION_SKIP_TRACE=<substring>`** (Shape::Draw): one dump per
   matching shape of every section — texture, Special flags, and skip reason
   (HIDDEN / GPUOWNED / FILTER). Separates "section skipped at submission"
   from "submitted but lost later".
3. **`WGR_LEGACY_TEXTURE_TRACE=1`**: per-texture draw parameters (alphaClass,
   alphaRef, blend, depth, camera, cmd index). CAUTION: dedups by texture
   name, so a line can be a stale first-use from another context (the planar
   reflection); and it proves SUBMISSION, not arrival.
4. **`WGR_PLAN_TRACE=1`** (Rust): one line per frame of the render plan's
   segment structure (`terr grass 3dx155 CLEARDEPTH 3dx66 RESOLVE 2dx20`).
   Kills pass-ordering theories wholesale.
5. If submitted with sane state and still absent: the loss is in a GPU stage
   no draw-side door reaches — **clipping (projection!), viewport, scissor,
   depth-test against the wrong buffer, camera slot**. MAT-052 lived here.
   Check every `GEngine->` hook the draw path calls between submission and
   rasterisation — a virtual with a no-op default that GL33 overrides and
   wgpu does not is EXACTLY the shape of this bug
   (`grep "virtual void.*{}" Engine.hpp`, then diff overrides GL33 vs Wgpu).

## Bisect recipe (unattended, ~10 rounds for 600 commits)

`git bisect run` with an oracle script: incremental ninja build → timed
capture → pixel judge → exit 0/1/125. Validated endpoints first.

- **Trigger captures by TIME (`--auto-screenshot 20s:...`), never by frame**:
  GL33 runs the witness scene at 1.7 fps, so `500:` compares different game
  times — different sun, different weather — and every brightness comparison
  is garbage.
- Oracle skips (125) on build failure; expect a handful mid-stack.
- **After the bisect: clang PCHs are stale and ccache is POISONED.** Delete
  every `cmake_pch.cxx.{pch,obj}` pair and rebuild ONCE with
  `CCACHE_DISABLE=1`, or ctest will greenlight stale test executables while
  the build errors scroll past.

## The two lies that cost the most

- **A change that darkens the backdrop looks like a fix.** `WGR_MSAA=1` and
  `WGR_ABLATE=clouds` both "restored" the cockpit — they only removed the
  bright cloud deck behind/over it. Verify geometry PRESENCE (rivets, texture
  detail at 5x zoom), never pixel brightness alone.
- **A surface lit to sky brightness is a perfect invisibility cloak.** A plate
  rendering at 195 in front of a 200 sky reads as a HOLE — "transparent, but
  only where the sky is behind it; looking down it is fine" (the owner's exact
  words, and the sentence that ultimately named the bug). The kill door cannot
  distinguish "not drawn" from "drawn at sky brightness" there: killing the
  draw changes the pixel by almost nothing. Pin `WGR_AUTO_EXPOSURE=0` for
  every kill comparison — killing draws changes average luminance and the
  exposure re-brightens everything, polluting every delta.

## MAT-052 turned out to be THREE stacked mechanisms

One symptom, three causes, each masking the next. Fixing one and re-testing
was the only way through; no single theory explained all the evidence because
no single mechanism produced it.

1. **Near-clipping** (`7e9693c4`): wgpu had no `UpdateProjection` override, so
   the cockpit's `SetClipRange(0.01, 50)` bracket never reached the GPU and
   everything nearer than the world near plane was clipped. Restored the
   dashboard and door pillars.
2. **God-rays over stale depth** (the "transparent where sky behind" itself):
   `frame_depth_final_resolved` is set at the seg-0 cloud block with the claim
   "all 3D depth is written by now" — false for the frame, because the
   post-ClearDepth cockpit segment rewrites depth after it. The god-ray march
   read SKY depth at every canopy-frame pixel and additively painted
   full-length sun shafts over the airframe. Fix: clear the flag when a later
   segment writes depth, so the march re-resolves after the LAST depth writer.
3. **Sky-dome ambient on interiors** (the residual wash): a vehicle interior
   is enclosed by geometry no ambient occluder can see (sky-vis is
   terrain-keyed, GTAO reaches ~2 m, interior volumes are baked for buildings
   only). Cockpit-flagged draws (`WGR_DRAW3D_COCKPIT`, from the engine's
   existing `PassKindHint::Cockpit` scope) now take 0.18 of the sky wash.
   TRAP: the first attempt branch-switched cockpit draws to the legacy flat
   ambient and turned every INSTRUMENT FACE black within the hour — dial
   materials carry no ambient of their own and live off the sky term. Damp
   the term; do not switch branches.

## The instrument that finally separated layers: paint-and-read-a-channel

When a pixel's value would not respond to ANY shading change yet died to the
engine-side kill, the winning probe was: hard-paint the suspect draw's
fragments PURE MAGENTA `(4,0,4)` in the fragment shader, then read the GREEN
channel at the pixel. Green is zero in the probe, so any green measured is
light added ON TOP by a later pass — its amount is the overlay's contribution,
separated from the surface in one capture. (Beam pixel: 255,195,255 → a
~195-bright overlay = the god-ray pass. Dash: 255,0,255 → bare fragment.)
Restrict the probe to opaque fragments or a full-screen glass pane drowns it.

## Standing constraints for cockpit work

- OFP cockpits draw as a post-`ClearDepth` segment with their own projection
  (`Transport::Draw` zSpace bracket, TransportCore.cpp:~1905). ANY pass that
  claims "depth is final" mid-frame is wrong once a cockpit exists; any
  depth-resolve consumer must account for the second depth clear.
- The engine's `PassKindHint::Cockpit` scope is carried to the shader as
  `WGR_DRAW3D_COCKPIT` (bit 4 of Material.normal_map.w). Use it for any
  interior-specific shading rule instead of inventing a new scope.
- Cockpit normals may be authored for the exterior; shade() faces them at the
  viewer for cockpit draws (same select as the foliage flip).
- Frame-count screenshot triggers compare different GAME TIMES across
  renderers (GL33 runs the witness scene at 1.7 fps); trigger by time
  (`--auto-screenshot 20s:`).
- After a `git bisect`, clang PCHs are stale and ccache is POISONED: delete
  every `cmake_pch.cxx.{pch,obj}` pair and rebuild once with
  `CCACHE_DISABLE=1`, or ctest greenlights stale test executables.
