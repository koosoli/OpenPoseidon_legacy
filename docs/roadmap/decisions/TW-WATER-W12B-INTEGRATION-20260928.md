# Tidewater W12b integration, 28 September 2026

The one missing commit from `dec/tidewater-w12b-spray-ab` is integrated as `d6b29906`, patch-equivalent to upstream `d56f91b3`. `WGR_TW_SPRAY_EARLY=1` restores the pre-W12a spray ordering for A/B captures; unset, W12a draws spray after clouds. W11e `975d4aff` and W12a `0e4e0dfb` were already integrated immediately before it. The follow-up `1b2cc5fa` avoids opening the late spray pass when spray is disabled or the early A/B order is selected. It does not change the normal spray simulation or water backend default.

The combined desktop build, 54 focused Tidewater Rust tests (one pre-existing ignored), and 45 ship/source-revision C++ assertions passed. Paired installed WGPU startup, a short showcase smoke and matched performance remain to be verified. Tidewater Native remains the water default; the experimental GeometryPage streamer remains off and cannot release Fine pages.
