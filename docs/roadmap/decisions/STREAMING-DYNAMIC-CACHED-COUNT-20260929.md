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
The paired desktop PoseidonGame/PoseidonTests build passed. The committed
`5a2ae871` EXE/DLL pair passed installed WGPU `--check` and a single
90-second native Reforger Village route: normal exit, 20,683 registered
instances, 4,930 main-pass instances and one captured 35.531 ms frame. This
is a stability screen with no matched baseline or causal FPS conclusion.
The original-source `-MainCountProbe` fixture
`dynamic-count-20260929-20260929-143604-d29dc4` passed its geometry
contract, main/solar COUNT logging and normal shutdown. Its first launcher
attempt failed before game start because Windows PowerShell did not expose
`Get-FileHash` through Git Bash; invoking the installed PowerShell 7 runtime
under the same game lock ran the fixture successfully.

## Skin-source follow-up

The next narrow source audit found two more ways to alter an image without an
instance-row change. `mesh_set_skin` can replace a mesh's bone/weight buffer;
the per-frame palette upload can move a skinned caster. Both now advance the
same optional, conservative target revision after a successful upload. The
ordinary unarmed path does only the existing Option check. Exact mesh ownership
is not available here, so an armed target may be invalidated by a foreign
skinned mesh; this is safe but can lower diagnostic coverage. The rigid opaque
GeometryPage pilot is unaffected by the extra invalidation.

The existing revision-transition test and paired desktop game/test build passed.
Installed WGPU verification for this follow-up is pending its committed build.
Bindless texture replacement and other material changes remain unwitnessed;
the cached image and Fine-release gates therefore stay closed.
