// Under fire (swim-underfire.Eden): the player in god mode (allowDamage false) in ~5.5 m of water off the Everon bay;
// four East riflemen on the beach fire into the water around him and grenades drop in near him (grenade.sqs). He dives and stays
// under for 24 s of fire -- longer than one breath would be worth without god mode. He must come through unhurt
// (hits, blasts and drowning all go through the guarded SetDammage), and the fire must really have happened.
_keyZ = 29
uwShots = 0
{_x addEventHandler ["fired", {uwShots = uwShots + 1}]} forEach [e1, e2, e3, e4, g1]
triSimFrames 120
triScreenshot "01_surface"

// a tap dives, held he sinks on
triKeyDown _keyZ
triSimFrames 1
triKeyUp _keyZ
triSimFrames 60
triKeyDown _keyZ
triSimFrames 90
triKeyUp _keyZ
t0 = time
triSimUntil { time > t0 + 8 }
triScreenshot "02_under_fire"
triSimUntil { time > t0 + 16 }
triScreenshot "03_under_fire"
triSimUntil { time > t0 + 24 }
triScreenshot "04_under_fire"

triAssertEq [alive player, true]
triAssertLt [damage player, 0.01]
triAssertGt [uwShots, 15]
triAssertGt [uwGrenades, 2]
triEndTest
