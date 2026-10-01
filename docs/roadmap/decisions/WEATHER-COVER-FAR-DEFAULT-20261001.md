# Default distant weather coverage

The corrected original-Eden campaign on installed 3353770c is retained at
`build/weather-cover-performance/stock-loader-far-cover-cost-20261001-044220-e21878`.
All twelve OFF/ON/ON/OFF launches exited normally with the same binary hashes
and exact populations: 52,507 registered objects, 3,064 main instances,
6,311 main records and 719,070 main triangles.

OFF whole-frame GPU samples in milliseconds were 7.839744091, 8.337408066,
8.338432312, 8.617983818, 7.332863808 and 8.042495728. ON samples were
8.428544044, 8.815615654, 8.199168205, 8.437760353, 7.563263893 and 7.533567905.
Medians are 8.189951897 and 8.313856125 ms: +0.123904228 ms, or +1.512881%.
Spread overlaps and there is block drift. This is a bounded same-scene
whole-frame comparison, not a dedicated pass timestamp or an all-scene budget.

Root's delegated visual judgment accepts this measured cost for the distant
white crowns and roof coverage seen in the separate original-map fixtures.
Far coverage now defaults on, with exact `WGR_WEATHER_COVER_FAR=0` disabling it.
Exact 1 enables it; unknown supplied values remain disabled. The master OFF
switch dominates. Existing activity gating allocates and draws it only with
finite active snow or sufficient wet ground, and releases it when inactive.
The extra 2048 Depth32Float texture is 16 MiB by source, not measured total
VRAM. Near 640 m precision, far 1600 m extent and all source/receiver guards
are unchanged. This extent does not cover every possible view-distance ray.

The forest runner's `-DefaultFarCover` removes the override entirely, proving
the production default rather than forcing 1. Default GPU/full-build and
installed Eden/Noe visual checks remain mandatory before a shipping claim.
