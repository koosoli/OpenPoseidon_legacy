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

The optional `-ManualPilot` mode selects the actual stock pilot as player, uses
the existing AUTOHOVER action, and preserves helicopter control with a scripted
camera. A short ordinary SDL Q pulse refreshes the keyboard height helper after
each single stage relocation. It does not set rotor RPM, velocity or simulated
particles, or continuously pin aircraft position. Actual flight position, engine
state, emission trace, source surface, pause and particle-budget gates still
apply. Parser verification alone does not accept this new mode; actual flight
and dust/snow appearance checks remain pending.

The accepted water downwash producer and renderer are unchanged.
