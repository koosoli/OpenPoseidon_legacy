# Land rotor flight fixture follow-up

Installed cff615b3 attempts `first-installed-20261001-011952-b9a78d`,
`object-display-fixed-20261001-012052-0ed98d`,
`actual-flight-state-20261001-012647-96b2b6` and
`clear-sand-patch-20261001-012930-0a3cbc` remain failed. Every owned process
exited normally with unchanged binary hashes.

The first attempt failed decoding the SQF aircraft display as JSON; the runner
now explicitly formats object identity as a string. Later runs did not establish
the required rotor trace. Recording actual position and engine state diagnosed
fixture behavior: the AI turns the engine off when commanded to its current
position, or the aircraft leaves the eight-metre source footprint. None proves
that land emission works. The original sand site also has nearby stock geometry;
the clear-site run used actual source-admitted SandDark at2700/5150.

The optional `-ManualPilot` mode boards the actual mission player as pilot, uses
the existing AUTOHOVER action, and uses ordinary external camera during flight.
Render-only view overrides apply only to paused screenshots and are then cleared.
A short ordinary SDL Q pulse refreshes the keyboard height helper after
each single stage relocation. It does not set rotor RPM, velocity or simulated
particles, or continuously pin aircraft position. Actual flight position, engine
state, emission trace, source surface, pause and particle-budget gates still
apply. Parser verification alone does not accept this new mode; actual flight
and dust/snow appearance checks remain pending.

The first controlled-pilot attempt used unsupported original OFP `selectPlayer`.
The game reported a script abort and timed-out harness command. Its failed
cleanup required terminating only the owned test process; this attempt is a
failure, with no ordinary-shutdown acceptance. The correction uses the existing
mission player rather than inventing a replacement command. The subsequent
`original-pilot-board-20261001-015327-ad03fa` exits normally but still records
engine-off behavior and no required emission trace. Input/control diagnosis
remains open; neither attempted manual setup accepts downwash appearance.

Source diagnosis of the second manual attempt found that a scripted camera effect
sets `World::_playerSuspended`, routing a manual helicopter driver through
`SuspendedPilot` instead of the keyboard collective path. The revised mode
requires `triGetCameraEffectActive=0` before actual input. Its installed hover
acceptance remains pending.

The accepted water downwash producer and renderer are unchanged.

The installed `50db754c` read-only state campaign
`actual-rotor-authority-20261001-022344-c46f58` resolves the manual fixture's
engine shutdown. Actual pilot/focus/HeliPilot authority and unsuspended Q input
are correct. The paused start has zero rotor RPM; after relocation to18m the
held Q query reports `moveUp=1`, engine on and RPM0.014. The released query
reports RPM0.016. The aircraft falls, reaches damage1/destroyed, and shuts its
engine off. This failed campaign exits normally with the installed pair intact;
it is not a land-particle acceptance.

The runner now lets the actual undamaged aircraft spool on its ground support
before any hover relocation. It requires engine on, damage0, clearance below6m
and actual RPM at least0.85, recording the simulated elapsed time. No RPM or
velocity setter is used. The new gate and sustained hover/particle checks still
require an installed run.
