# Normal-hover rotor land optics and draw attribution

The installed `e017` campaign `actual-land-dust-snow-strength-20261003-141948-c55a65` remains visually rejected: its dry-sand and deposited-snow pictures do not show a readable ring. Emission and live-puff receipts establish source admission and lifetime, not visibility. This candidate still requires a full build and installed normal-hover visual review.

## Measured optical input

Actual manual stock UH60 hover records have height 19.45–19.82 m and RPM 1. The existing physical attenuation `(1-height/30)^2` consequently gives roughly 0.115–0.124 density. The previous linear optical curve gives dust alpha 0.0805–0.0868 and snow alpha 0.0690–0.0744 before entry/exit fades, sheet alpha, depth fade and fog. The stock sheet mean alpha of approximately 0.32 is cited by the previous source audit; it was not independently measured in this change. That estimate puts the old sheet-mean opacity near 2–3%, consistent with the unreadable pictures but not proof of the complete GPU cause.

The private land-puff optical calibration now uses `sqrt(clamp(density,0,1))` with the same 0.35 dust / 0.28 snow opacity caps, 0.70 / 0.60 gains, radius, lifetime and entry/exit fades. At those actual hover densities, unfaded dust alpha becomes 0.2374–0.2465 and snow 0.2035–0.2113. The physical density, emission floor, particle budget, terrain provenance, wet-source suppression, roof rejection, high-altitude/engine-off gates and original water downwash are unchanged. These are bounded authored optical gains, not a claim that pixels now pass visual acceptance.

## Draw-path evidence and diagnostic scope

Source audit found the dedicated land puff uses the stock animated sheet through one-face CloudletBasic drawing. Smoke-shadow substitution is explicitly disabled. Its culling sphere already covers the full square diagonal. Its projection, mip selection, material packing and fog inputs retain the existing DrawDecal conventions. `triSetView` changes the actual scene camera through World simulation; it is not a second renderer-only camera. No specific draw-admission failure has yet been proven.

`POSEIDON_ROTOR_LAND_DRAW_TRACE=1`, or the installed runner's optional `-DrawTrace`, records actual land-puff Draw early exits and actual CPU DrawDecal calls. It reports counts and finite ranges for packed alpha, projected half extents in pixels, metre radius, camera distance and physical age, plus live count, scene-cloudlet state, current physical clock and scene camera. The trace is off by default. The off path does no diagnostic accounting, clock reads or distance calculation beyond checking the cached flag.

Reports aggregate **all land puffs** since the preceding report, at most once per wall second. Wall cadence allows observations during paused rendering without advancing physical age. The report's clock and camera are current report values; they are not exact identities of every draw in the aggregate or the screenshot frame. `decal_calls` proves CPU invocation only, not backend admission, rendered pixels or final visibility. An emission-side report allows live puffs with zero Draw calls to be distinguished from a successful Draw submission. No waits, GPU readbacks or state mutation are added.

The runner saves, clears and restores the new process variable alongside its existing environment isolation. Default runs leave it disabled. Existing simulation timings and strict physics/source checks are retained.

## Checks and remaining acceptance

The focused native RotorGroundPolicy check passed with Clang. Tests retain physical guards, bounded caps/radius/fades and add the actual 0.115 and 0.124 optical cases for both media. Pure receipt tests reject nonfinite payloads, distinguish early exits from Decal calls, verify finite min/max ranges, and reset the window correctly. The installed runner self-test covers the actual bound audited sea pair and existing source/authority falsifiers without starting a game.

The root agent owns full-build verification and the installed campaign. Run the existing strict stock Noe land fixture with `-ManualPilot -SandX 2475 -SandZ 5075 -SeaX 1750 -SeaZ 3100 -DrawTrace`. Preserve installed pair hashes, actual helicopter/crew identity, actual neutral hover, roof/wet/high/engine-off and sea negative controls, and normal exit. Examine draw calls, early exits, packed opacity and projected sizes alongside close and top images. Accept dust and snow quality only from readable actual normal-hover pictures; retain the existing original water appearance. No all-map or performance acceptance is inferred from this focused diagnostic.
