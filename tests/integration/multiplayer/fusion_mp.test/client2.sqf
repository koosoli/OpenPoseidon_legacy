// WEST:2, p2 on Everon
triAssertNgsClient 14
triAssertMissionPlayable
triAssertEq [triPlayerAuthority, "OK"]
triAssertEq [player == p2, true]
triAssertGt [(triTerrainHeight [28800,22400]), 0]
triAssertGt [(triTerrainHeight [78660,64700]), 0]
triAssertGt [(triTerrainHeight [60800,35200]), 0]
triAssertGt [(triTerrainHeight [17364,54008]), 0]
triAssertNear [(getPos p2 select 0), 78660, 30]
triAssertNear [(getPos p2 select 1), 64700, 30]
triAssertNear [(getPos p1 select 0), 28800, 30]
triAssertNear [(getPos p1 select 1), 22400, 30]
triAssertGt [(p1 distance p2), 50000]
triAssertEq [alive heli1, true]
triAssertGt [(getPos heli1 select 2), 20]
triScreenshot "client2_everon"
fusionSeen = 1
publicVariable "fusionSeen"
// client1 teleports p1 to Nogova; the move must reach this client
triAssertNear [(getPos p1 select 0), 17364, 30]
triAssertNear [(getPos p1 select 1), 54008, 30]
triAssertGt [(p1 distance p2), 50000]
fusionMoveSeen = 1
publicVariable "fusionMoveSeen"
triWait 3000
triEndTest
