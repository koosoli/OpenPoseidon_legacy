// Sinkhole W4: diving. The player in deep water off the Everon bay (swim-bay.Eden) dives on move-down (Z),
// sinks further while Z is held, holds his depth with no keys, and surfaces on move-up (Q) back into treading.
// getPos height over the sea is from the surface; a man treading water has his origin ~1.5 m under it, and the
// swell moves the surface +-0.4 m, so the margins below are generous.
_keyZ = 29
_keyQ = 20
triSimFrames 120
_z0 = (getPos player) select 2
triAssertGt [_z0, -2.0]
triScreenshot "01_treading"

// the dive, then on down while Z stays held
triKeyDown _keyZ
triSimFrames 90
triScreenshot "02_diving"
triSimFrames 270
triKeyUp _keyZ
triSimFrames 30
_z1 = (getPos player) select 2
triAssertLt [_z1, _z0 - 1.5]
triScreenshot "03_under_water"

// holds his depth with no keys (the body does; the waves pass over him)
triSimFrames 120
_z2 = (getPos player) select 2
triAssertLt [abs (_z2 - _z1), 0.6]

// up and out
triKeyDown _keyQ
triSimFrames 480
triKeyUp _keyQ
triSimFrames 150
_z3 = (getPos player) select 2
triAssertGt [_z3, _z1 + 1.0]
triAssertGt [_z3, -2.0]
triScreenshot "04_surfaced"

triEndTest
