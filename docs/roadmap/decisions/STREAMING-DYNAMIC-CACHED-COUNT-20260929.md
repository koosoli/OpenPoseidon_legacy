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
The committed `a0ba77e9` EXE/DLL pair passed installed WGPU `--check`.
## Texture-source follow-up

Object textures may replace the view behind a leased bindless slot without
changing a model's baked slot number. The Renderer now conservatively advances
the optional target revision after a successful texture creation/replacement,
accepted RGBA write, destruction of a live handle or release of a live lease.
Refused or stale operations do not advance it. A replaced old upload remains a
live handle until retired, so retiring it can advance the witness even though
the newer view stays bound; generic overlay textures can also cause a safe
false invalidation. This favors Unknown over reusing a stale shadow fact.

The normal path performs only the optional target-witness check after existing
texture operations; it adds no scan, GPU copy or readback. Focused texture
tests passed (17/17), including valid/refused mutation results and real
bindless replacement cases. The paired desktop game/test build passed.
The committed `bf76fa15` EXE/DLL pair passed installed WGPU `--check`.
Non-texture
material edits and other image producers are still untracked, so no cached
image, Fine-release or default GeometryPage policy is promoted.

## Rigid target and foreign palette images

`a525fef8` separates full bounded palette-image mutation from an armed target
COUNT revision. The revision survives a foreign pose change only when the
actual current target is proven rigid within bounded model/mesh/section
ownership checks; stale, missing, skinned or oversized targets invalidate.
Palette bytes, buffer identity, slot order and stride remain in the distinct
image serial and cached image keys. Allocation/cap/refusal discards the image
baseline. Twelve focused Rust tests and the paired build/startup passed.

Installed `count-palette-independent-sep29-20260929-234146-510de6` exited
normally but its harness verdict failed: AdmitFine returned producer model4
while renderer model3 still belonged to the previous Pending request. The
log actually contains current main plus solar0–3 Present1 for coarse token1
and Fine token3, with source generations4/5 and epochs4/6 respectively.
The runner now requires exact returned Ready before checking its COUNT log.
The failure artifact is preserved; a repaired verdict remains pending.
No cached-image, all-view-zero, automatic Fine-release or performance gate
was relaxed.

The repaired installed `a525fef8` run
`count-palette-returned-sep30-20260929-235938-a6e2bc` passed, exit0,
normalShutdown=true and forcedTermination=false. Coarse/Fine/fresh refill
returned exact producer=renderer models3/4/5 before COUNT checking. Each
observed current main plus solar0–3 Present1; the last refill token9 also
reported planar reflection Absent0. This confirms the narrow rigid-target
positive path. It does not close cached/local/interior/GI all-view absence.
