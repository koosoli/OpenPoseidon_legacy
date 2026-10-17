// Sinkhole W4: diving. The player in deep water off the Everon bay (swim-bay.Eden) dives on a TAP of move down (Z)
// -- the way prone is pressed; a one-frame press must not be missed -- sinks further while Z is held, holds his depth
// with no keys, and a tap of move up (Q) takes him back up to the surface into treading.
// getPos height over the sea is from the surface; a man treading water has his origin ~1.5 m under it, and the
// swell moves the surface +-0.4 m, so the margins below are generous.
_keyZ = 29
_keyQ = 20
triSimFrames 120
_z0 = (getPos player) select 2
triAssertGt [_z0, -2.0]
triScreenshot "01_treading"

// a tap dives
triKeyDown _keyZ
triSimFrames 1
triKeyUp _keyZ
triSimFrames 90
triScreenshot "02_diving"
triSimFrames 120
_z1 = (getPos player) select 2
triAssertLt [_z1, _z0 - 0.8]

// held, he sinks on
triKeyDown _keyZ
triSimFrames 150
triKeyUp _keyZ
triSimFrames 30
_z2 = (getPos player) select 2
triAssertLt [_z2, _z1 - 0.5]
triScreenshot "03_under_water"

// holds his depth with no keys (the body does; the waves pass over him)
triSimFrames 120
_z3 = (getPos player) select 2
triAssertLt [abs (_z3 - _z2), 0.6]

// a tap of Q takes him up and out
triKeyDown _keyQ
triSimFrames 1
triKeyUp _keyQ
triSimFrames 600
_z4 = (getPos player) select 2
triAssertGt [_z4, _z2 + 1.0]
triAssertGt [_z4, -2.0]
triScreenshot "04_surfaced"

triEndTest
