# Rotor-water supported shutdown fixture

The installed `original-nogova-sea-rotor-scalar-20261003-123920-834a82`
Current OP arm passed low/high/return/dry hover and paused sea captures but failed
its shutdown fixture. Its final ground relocation preserved the manual flight
height target at 20.898 m. During actual engine-off rotor decay, vertical speed
rose from 2.14 to 4.52 to 5.76 m/s and clearance from 2.84 to 4.46 to 6.71 m at
RPM 0.942, 0.859, and 0.767. The strict clearance-below-6-m gate correctly failed.
This was an invalid supported shutdown control, not evidence that water rotor
effects failed.

`HelicopterAI.cpp` keyboard control explains the result: collective down
initializes `_pilotHeight` from actual height, then lowers it by
`20 * input * deltaT`, bounded by a stock landing floor of 1 m when horizontal
speed permits landing. Autohover only changes horizontal helpers. Moving the
aircraft to ground does not clear its manual height target.

`Test-RotorWaterRuntime.ps1` now holds ordinary stock Z / SDL scancode 29 after
the dry hover. Every actual sample must retain a healthy, local, occupied,
controlled helicopter and actual collective-down input. Three consecutive
samples must show `airborne=false`, clearance within [0,6) m, vertical speed
at most 0.25 m/s in magnitude, and the real height-helper target at its 1-m
landing floor. The test then pauses and switches off the engine on this actual
support. It keeps the same down input during real spool-down and retains the
strict support, aircraft identity, and engine-off checks until RPM <= 0.02.
No ground teleport, velocity assignment, RPM setter, or helper setter is used.

The former final sea camera observed a stopped helicopter on dry ground over
2 km away. A zero descriptor there could be caused by domain/location exclusion.
The new control releases Z, relocates the already stopped aircraft once into the
same actual low-sea admission window, and allows a brief ordinary simulation
step. The recorded time must advance, altitude must fall, and vertical speed
must be negative. The test pauses at 8–28 m above actual sea, with engine off,
zero fuel, RPM <= 0.02, the same controlled healthy aircraft, underwater native
bed, and position within the existing 8-m sea fixture bound. The final fresh
copied-UBO zero receipt therefore observes a stopped rotor inside the same
256-m camera domain and low-altitude water window. This is explicitly a falling
engine-off control, not an unsupported claim of engine-off hover.

All previous hover, source/UBO clock/domain, pause, backend, process ownership,
binary identity, and normal exit gates remain. Cleanup releases Z when held.
CPU `-SelfTest` now also rejects airborne, climbing, high-helper-target,
uncontrolled, dry-bed, high-altitude, spinning, and out-of-domain shutdown
surrogates. Source-only self-tests and diff checks pass; root owns the actual
installed-game landing, shutdown, gravity and OP/Tidewater smoke tests.
