# Weather flight performance attribution

The real installed b59f455a altitude comparison reduces high-camera liquid
particle phase medians by about0.2ms. It does not explain or close the owner's
reported large helicopter-in-rain slowdown. Source inspection found physical
weather-map cull/draw work outside the existing interior-sky timing brackets.

Append-only timing rows84..87 now name near/far physical weather cover cull and
draw. Each wraps its actual existing cascade once; no pass/admission/publication
changes. CPU encode brackets use the existing timer clock; GPU results use the
existing nonblocking timestamp ring. Missing/inactive rows remain unavailable.
Both headers and Rust share88 regions; compile-time equality also repairs the
previous backend-agnostic cap81 which silently omitted TW rows81..83.

The harness `frame_performance` command reads existing published renderer rows
and at most256 completed CPU main-loop records. It neither waits for GPU nor
resets capture/timers or mutates rendering. It emits runtime version, dimensions,
MSAA/upscaler, per-row index/name/GPU and CPU milliseconds plus nesting, and CPU
window phase averages/p95/max. Rows can lag game state and are not same-frame
completion or unique-frame identity proof. Negative values mean unavailable.
`weather_visibility` additionally reports actual overcast so rain/dry flight
comparisons cannot silently attribute a different cloud setting to rain drops.

Required runtime: occupied stock helicopter with actual input, RPM, position,
velocity and pilot authority; independent dry/rain arms with matched cloud
settings, ordered CPU capture without drops and bounded published-region polls.
Measured route variation must be reported. No synthetic position/RPM during the
measurement interval, no forced frames or paused-camera claim of flight cost.
Optional renderer ablations diagnose cost only, not acceptable final appearance.

Full combined renderer device checks/build and installed runtime are pending.
