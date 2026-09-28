# Tidewater W11b integration, 28 September 2026

Oliver requested the missing commits from `dec/tidewater-w11b-boat-planing` while streamer work continues. The isolated port compared this branch against the current W10g tip and found exactly two new patches. W11a (`71600cda`) uses the interpolated rendered hull transform for wake and bow spray, aligning them with the hull drawn in the same frame. W11b (`b993b447`) reduces small-boat planing lift at speed so the hull remains submerged while scaling drag to preserve the previous approximate speed response. `POSEIDON_BOAT_LEGACY_PLANING=1` restores the old lift. Both patches replayed without conflict; `git cherry` found no further missing source patch.

The combined desktop build passed; focused C++ ship tests passed 10 assertions in two cases. These tests do not establish the visual wake alignment, boat waterline, handling equivalence or matched frame-time behavior. The streamer GeometryPage default remains off with Fine release prohibited. Installed runtime check and Oliver's interactive judgement remain pending at this checkpoint.

Subsequent W11c/W11d source updates are recorded separately in `TW-WATER-W11D-INTEGRATION-20260928.md`; their 9x/12x gain supersedes the W11b 20x/26x default described above.
