# Foreign dynamic casters invalidate private cached COUNT (29 September 2026)

The selected-model revision and retained-static epoch do not change when an
unrelated dynamic instance moves. A cached shadow or GI image may then still
have the same target-model COUNT inputs and static epoch while its foreign
casters have changed. The private cached COUNT getter previously had no
source witness for that case and could retain an old `Absent` fact.

An explicitly armed COUNT request now records the existing optional full
dynamic-row mutation serial. Publication of a local shadow, interior sky or
GI RSM COUNT is refused if those rows changed before commit. Each published
cache key carries that serial, and a later lookup or diagnostic candidate
requires the current serial to match. The 36-view aggregate also returns
Unknown if dynamic rows changed since its request was armed. Moving a foreign
soldier or vehicle therefore cannot preserve an old cached absence merely
because the selected model and static instance epoch stayed equal.

This source-local witness is armed only for a private COUNT request. The
ordinary renderer path has no new row comparison, allocation, GPU copy or
readback. This remains diagnostic only: mesh/material/texture/pose changes,
pixel visibility, physical Fine reclaim and matched performance are still
unproven. GeometryPage and pressure control remain default-off.

Focused cached-count tests passed (16/16), including stale foreign-caster
absence in local, interior and GI caches; 14/14 view-reference tests passed.
The paired desktop PoseidonGame/PoseidonTests build passed. Installed WGPU
startup and a representative streamer world run are pending this commit.
