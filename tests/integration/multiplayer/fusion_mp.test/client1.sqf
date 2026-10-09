// WEST:1, p1 on Malden
triAssertNgsClient 14
triAssertMissionPlayable
triAssertEq [triPlayerAuthority, "OK"]
triAssertEq [player == p1, true]
// terrain: land on every island
triAssertGt [(triTerrainHeight [28800,22400]), 0]
triAssertGt [(triTerrainHeight [78660,64700]), 0]
triAssertGt [(triTerrainHeight [60800,35200]), 0]
triAssertGt [(triTerrainHeight [17364,54008]), 0]
// own and remote unit positions (Malden <-> Everon, 65 km)
triAssertNear [(getPos p1 select 0), 28800, 30]
triAssertNear [(getPos p1 select 1), 22400, 30]
triAssertNear [(getPos p2 select 0), 78660, 30]
triAssertNear [(getPos p2 select 1), 64700, 30]
triAssertGt [(p1 distance p2), 50000]
// AI helicopter: alive, airborne, moving
triAssertEq [alive heli1, true]
triAssertGt [(getPos heli1 select 2), 20]
heliStart = getPos heli1
triWait 10000
triAssertGt [abs((getPos heli1 select 0) - (heliStart select 0)) + abs((getPos heli1 select 1) - (heliStart select 1)), 100]
triScreenshot "client1_malden"
// once client2 has seen p1 on Malden, jump to Nogova (61 km from Everon)
triAssertEq [fusionSeen, 1]
player setPos [17364,54008,0]
triAssertNear [(getPos p1 select 0), 17364, 30]
triAssertNear [(getPos p1 select 1), 54008, 30]
// stay connected until client2 has seen the move
triAssertEq [fusionMoveSeen, 1]
triEndTest
