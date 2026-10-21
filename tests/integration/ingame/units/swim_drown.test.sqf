// Sinkhole W4: no endless diving. The player dives (a tap of Z) and stays under: after 40 s his air is gone and
// he drowns over the next 12 s, whatever his armour. Waits on mission time, not frames (the frame rate varies), and
// stops at 90% damage: the player's death ends the mission and the game with it.
_keyZ = 29
triSimFrames 120
triAssertLt [getDammage player, 0.1]

triKeyDown _keyZ
triSimFrames 1
triKeyUp _keyZ
swimDiveT0 = time
// 30 s under: unhurt
triSimUntil { time > swimDiveT0 + 30 }
triAssertLt [getDammage player, 0.1]
// drowning: 90% damage comes after the 40 s of air and ~11 of the 12 s drowning, and not much later
triSimUntil { getDammage player > 0.9 || time > swimDiveT0 + 70 }
triAssertGt [getDammage player, 0.9]
triAssertGt [time - swimDiveT0, 48]
triAssertLt [time - swimDiveT0, 56]
triScreenshot "01_drowning"

triEndTest
