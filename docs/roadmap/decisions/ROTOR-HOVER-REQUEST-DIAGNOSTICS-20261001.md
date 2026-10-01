# Read-only hover demand after real rotor spool

The matched installed 50db campaign `grounded-real-spool-20261001-022929-51993c`
proved real grounded spool (17.425 simulation seconds to RPM0.854), later RPM1,
approximately 10 m clearance, eight land puffs per emission and bounded 48 live
particles. The later observation left its eight-metre fixture footprint.
Root's detailed review of neutral freelook/wind controls in
`neutral-look-ground-spool-20261001-030423-ab0d44` confirms eight-second dry and
snow observations passed, each with four real emission batches of eight puffs
and live48. The full campaign failed later at the roof-covered stage, where the
roof changes aircraft physics. The overall failure must not be attributed to
the earlier dry hover. Neutral controls therefore have bounded dry/snow runtime
support; coverage/collision transitions still need independent diagnosis.

The prior getter established driver/manual/camera ownership, RPM, vertical key
state and actual speed, but could not distinguish hover off, requested horizontal
motion, missed freelook or physical drift. External camera is included by
`Object::IsGunner`: mouse-turn auto-hover can command `-mouseDirWanted.y*20`.
The keyboard hover branch instead requests zero horizontal speed with no move
keys; its speed helper must actually be active. `AUTOHOVER` toggles rather than
sets the state, so repeating it blindly could undo an already active hover.

This source slice adds a const diagnostic snapshot to Helicopter. Its base reports
unsupported, while HelicopterAuto copies actual hover, helper, desired speed,
height/heading/dive and mouse-direction fields. There is no class-layout/data
field change or unsafe assumption that every Helicopter is an automatic model;
the internal virtual interface changes and requires a combined game build.

`rotor_state` appends actual hover availability/state, the three helper states,
requested pilot-speed XYZ, mouse-direction XYZ, wanted height/heading/dive, camera
type, Alt key, mouse-turn/freelook/joystick states, context-filtered forward/fast/
back/turn/move actions and actual world wind XYZ. New scalar/vector values receive
finite guards before JSON serialization. No consuming action query, flight
setter, toggle, simulation update or control injection occurs.

Root should retain sparse snapshots after boarding, AUTOHOVER, ground spool,
Q release and observation drift. Compare requested speed against actual speed:
nonzero requested speed explains a control demand; hover false or speed helper
false invalidates a hover assumption; zero requested speed with an active helper
requires investigating the actual aerodynamic/world force path. Do not infer
zero wind solely from an environment setting or assert that Alt successfully
selected freelook from a sent key event.

The runner remains root-owned and unchanged. Whitespace/source contract checks
pass; no local full build, game run or GPU work was performed. Root must compile
the new virtual interface and repeat the installed read-only query. Particle
appearance, stable multi-stage hovering and accepted water downwash remain
separate runtime claims.
