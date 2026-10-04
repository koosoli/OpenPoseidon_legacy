# Owner-authorised automatic ragdolls by default

Oliver explicitly requested default-on automatic ragdolls with a Physics on/off
switch after the installed frozen-hit/grenade acceptance. Source `f29455a8`
changes absent `POSEIDON_AUTOMATIC_RAGDOLL` from false to true. Explicit `0`
remains disabled; the shared Dev/runtime setter overrides the startup setting.

Ctrl+F8 -> Physics -> **Automatic infantry ragdolls (experimental)** is the
existing live admission checkbox, now labelled plainly. Turning it off stops
new automatic corpses and retains active and frozen ones; it does not delete or
reset existing corpses. The choice lasts until restart. Existing local-player,
stock model, terrain/contact, motion and allocation/budget guards remain.

Paired Game/Tests build passed 17 steps; focused native checks passed 76 cases
and 14,480 assertions. Matching source `f29455a8` deployed at 10:29 local.
The revised installed runner's `-DefaultStartup -RuntimeToggle` removes the
environment variable entirely, requires actual default enabled status, then
uses the same setter as the Physics checkbox to disable/enable. It retains the
four-owner/budget, consumed-pose, active/frozen toggle preservation, disabled
new-death and zero-handle cleanup gates; `-FrozenProjectile` additionally keeps
the strict actual stock-ammo wake/local-transfer/movement/resettlement proof.

Installed acceptance and final owner handoff are recorded after execution below.
These author-run checks do not broaden model/stance/water/multiplayer acceptance.

Additional projectile attempt
`owner-default-on-ragdoll-toggle-20261004-102951` passed actual absent-override
default ON, startup OFF/ON through the shared setter, four active owners and
active/frozen preservation, including refusal of a new death while disabled.
Its optional strict projectile arm failed: received=2/wake=1, but local
transfers=0. The actual motion diagnostic reported local readback
8.172262 m/s / 86.082405 rad/s, beyond the existing 20 rad/s angular limit.
The unchanged helper restored that local motion and used its bounded aggregate
fallback (whole .272409 m/s / .864203 rad/s). The attempt does not establish
local shot/resettlement acceptance and is retained as a known quality limit.
No solver, collision, impulse multiplier or acceptance threshold was changed
for default promotion. A separate focused default/toggle campaign follows.

Focused default/toggle campaign
`owner-default-on-physics-toggle-20261004-103230` passed normal exit0 and unchanged
matched EXE/DLL hashes. Actual no-override default enabled; shared setter OFF/ON;
four independent articulated owners, budget fallback, active/frozen pose
preservation, disabled new-death refusal, persistent settled poses and zero
owner/body/joint cleanup all passed. The harness exercises the checkbox's
shared setter rather than clicking the ImGui widget. The additional failed
local-shot attempt above is retained alongside the passing scoped campaign in
`automatic-ragdoll-default-20261004-runtime.json`.

Final owner handoff `20261004-103342` left the matching installed game open
(PID10500). Startup ragdoll override absent; actual status enabled=1. Ordinary
Infantry controls, real SDL left-click Fired=1 / M16 ammo30->29, six targets and
bounded native-input aim alignment passed. Root inspected the clear target view.
No owner judgement or universal ragdoll quality approval is inferred.
