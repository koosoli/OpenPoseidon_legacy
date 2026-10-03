# Tidewater rotor-only normal bridge

Source candidate based on `8aca5c43`; root owns the combined build, GPU-device
checks, matched deployment and installed runtime comparison. This change is
separate from the liquid-population repair `074a2fef`.

## Source gap and repair

Current OP already emits an RPM-scaled water ripple impulse from the captured
spinning helicopter sources (including the occupied aircraft). Tidewater's
impact emitter deliberately rejects events without ammo mass, while its boat
wakes use a separate simulation. OP's interaction field is not read by
Tidewater. The result is a real backend gap; no map-name filter was found.

The existing current OP rotor event gains a semantic flag `1<<13`, in its
existing float flag lane. Its kind, position, radius, RPM-squared strength,
one-frame lifetime, downward velocity and zero ammo/foam values are unchanged.
The OP interaction shader already ignores unrelated flags. No public structure
size or ABI layout changes. Ammo, demo and boat events are not tagged.

The unchanged healthy sea admission conditions now live in the production pure
`RotorSeaWashEligible` helper: spinning RPM, sea-relative altitude [-2,30] m,
native bed <= sea+0.25 m. Invalid inputs additionally fail closed. This applies
to any map and to its actual sea datum; it does not infer water from material
colour or the aircraft's altitude above the seabed.

The backend manager forwards its existing per-frame interaction params to
Tidewater, resetting rotor descriptors **before** this frame's events, including
zero-event frames. It replays only params when the backend switches, never old
rotor packets. The consumer requires exact rotor+pending flags, finite packet
lanes, current water-clock timestamp, zero ammo/foam/lifetime, the existing
bullet-kind/downward velocity, radius [3,15] m and strength (0,1.1]. At most eight
sources whose influence overlaps the existing <=256 m camera domain survive.

The private `TwSurface` block appends ten vec4 lanes (160 bytes), growing 800 ->
960 bytes, with compile-time size assertion and tested offsets. Existing field
offsets stay identical; no new texture, binding, compute pass or allocation per
frame is introduced. Rotor-only annular normal slopes affect reflection and
refraction; they do not create impact plumes, boat wakes, aeration or geometric
waves. Broad/fine wavelength filtering uses the existing pixel footprint to
avoid distant sparkling. Distance tests precede the additional trigonometry and
filtering. Influence fades at the domain edge and its summed slope is bounded.

Paused identical clocks/descriptors produce identical ripples. The existing
interaction freeze bit retains rotor positions and phase; disabling the narrow
ablation clears them. `WGR_TW_ROTOR_WASH=0` disables only this new bridge; absent
or exactly `1` enables it, other values disable. It does not disable the accepted
current OP effect or any Tidewater boat/munition behavior.

## Checks and remaining acceptance

`scripts/Test-TidewaterRotorPolicy.ps1` passed with Clang warnings as errors and
standalone Rust `-Dwarnings`: actual production sea admission and four production
descriptor tests cover shifted sea datums, measured Nogova bed bounds, dry/high/
engine-off/invalid refusal, bounded slots, no boat/ammo aliasing, stale-frame
refusal, zero-event removal, pause, interaction freeze, ablation and fresh
backend state. These are CPU/source tests, not an installed hover measurement.

The complete `water_tw::` CPU suite passed: **59 passed, 1 existing ignored**,
including actual Naga composition for the water material, shared-UBO layout and
its shore, spray, wake and underwater consumers. The first compilation correctly
rejected the old 800-byte size assertion; the assertion and offsets now cover
the expanded 960-byte private block. No GPU adapter/device was requested.

Root must still validate the combined device pipelines and compare actual
occupied hovering aircraft over the measured Nogova sea at X1750/Z3100, with
current OP and Tidewater. Required controls: RPM/source presence, actual local
sea/bed, engine-off removal, dry land, high altitude, pause and return, backend
switch, narrow bridge off/on and no changed boat/munition effect. Appearance and
frame cost remain unaccepted until that run; this is not yet a claim that the
reported Nogova failure or rain-flight performance is fixed in the installed
game.
