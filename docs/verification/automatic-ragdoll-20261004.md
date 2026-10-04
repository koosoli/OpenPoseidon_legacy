# Automatic Box3D infantry death checkpoint

The automatic path is integrated into ordinary stock US infantry damage deaths
in local single player. It uses the existing Box3D backend, eleven component
bodies, ten joints and four anatomical hinges per admitted corpse. It captures
the actual corrected skin, CPU points and equipment transforms before the death
animation advances, then retains the final pose after releasing solver handles.
The earlier manual settled-corpse prototype remains available independently.

The candidate is **experimental and OFF by default**. Launch with `--dev`, open
**Ctrl+F8 -> Physics**, and enable **Automatic infantry deaths (experimental)**.
The switch lasts until restart. Turning it off preserves existing corpses and
stops new admission. `POSEIDON_AUTOMATIC_RAGDOLL=1` remains the startup option.

## Current owner feedback follow-up

Runtime-created settled corpses now retain their physical recipe after releasing
solver handles. Later real hits can recreate the bodies/joints at the retained
pose and consume the incoming impulse in the same wake call, before normal
simulation clears it. Existing motion, world/terrain and four-active budget
guards remain. Legacy frozen saves do not contain this runtime recipe.
Native floor setup is prepared at enable/first live simulation before combat;
it still costs about 16 MiB and 239 ms in the measured Everon attempt.
See [current installed evidence](weather-fog-corpse-feedback-20261004.md).

## Earlier installed author-run acceptance

Current `1418e425` fixes blast corpses freezing at the old 12-metre travel bound.
Installed inherited-trajectory, actual grenade-death and frozen save/load passed;
retirement now requires actual supported low-motion ground contact. See
`fog-ragdoll-owner-revision-20261004.md` for evidence and remaining limits.

Matching source `3de356cb` / original Everon:

- Four ordinary standing deaths created 44 actual bodies and 40 joints with
  independent collision families. The fifth took the authored budget fallback.
- All four fell and froze with zero solver handles. Full consumed points,
  palettes, equipment, spatial roots and bounds remained exact after more than
  31 additional simulated seconds. All four near screenshots show retained
  corpses and shadows; this does not certify every distant LOD or camera.
- Actual mission save, deletion as a control, and load restored four frozen
  owners without solver handles. Full visible poses agreed within 2 mm. Deletion
  subsequently left zero owners, bodies and joints.

Matching source `7d5c1e92` additionally passed the shared runtime toggle path:
OFF at startup, enable four deaths, disable without removing active owners,
retain frozen poses, reject a new death while disabled despite free capacity,
re-enable and clean up. The tested setter is the one used by the Physics checkbox;
the harness did not click the ImGui widget.

The repaired installed AK74 fixture also passed at `7d5c1e92`: an actual live
Eastern soldier with a loaded rifle fired five shots and killed the stock US
target. The death entered a fifth independent automatic owner, froze and
returned to zero owners/body/joint handles after deletion. No artificial damage
command was used in the rifle arm. Root inspected its retained corpse screenshot.

Curated receipts: `automatic-ragdoll-20261004-runtime.json`. Raw actual pose
arrays remain in ignored `build/automatic-ragdoll/`; hashes, owner bounds, point
counts, actual backend counts, screenshots and lifecycle observations are retained
without copying retail skin vertices into Git. Normal exit and unchanged matched
EXE/DLL hashes passed for the successful campaigns.

The full native physics/corpse suite passed **74 cases, 13,806 assertions** after
the native-floor and contact corrections. Actual Box3D controls cover nonplanar
native terrain triangles and bounded boot contact recovery, including the 25 mm
boundary. Source and native tests alone were not treated as installed acceptance.

## Corrections and limits

The first installed campaign refused all owners because a 12.5 m collision grid
discarded Everon's actual 6.25 m vertices. The correction preserves the native
2048-square grid and its closing edges. A second campaign admitted two owners;
the other two had 8.53/16.95 mm initial boot contact in the original stance.
Bounded boot overlap now resolves through real contact without moving the
captured skin or initial anchors. Other component clearances and the 25 mm
agreement between native and solver floors remain admission checks.

Preparing a missing shared floor takes **about 231-247 ms** and 16 MiB of source
height data on this Everon fixture; later deaths reuse it. This measured first-use
pause and the bounded model/ground domain are reasons to keep the default OFF.
No Box3D dependency upgrade, asynchronous floor preparation or broad performance
acceptance is claimed.

The earlier rifle fixtures failed before establishing a real shooter/kill.
Their failures remain in the receipts. Source review found that `createGroup
east` returns a null group when the mission has no Eastern centre. The repaired
fixture creates that centre and asserts the real shooter binding, weapon and
ammunition before the actual firing/kill proof. A `MIDDLE` stance-request campaign also passed the script, but
its actual initial CPU body points equal the standing control: **crouch/prone
acceptance is unproved**. Neither result is relabelled as gameplay success.

Unsupported models, multiplayer, water, ladders, attachments, unadmitted
excavations, excess budget, unsupported initial contact and high initial speed
keep the authored death. Full explosive flight, inter-corpse pile quality,
arbitrary mod rigs, interior cavity collision and all-map support are not
certified by this checkpoint. See the detailed PHY-040 decision record.
