triAssertEq [(triDisplay), 70]
triAssertEq [triMpAssignSelfSlot "WEST:1", "OK"]
triAssertNgsClient 13
triAssertEq [(triDisplay), 53]
triInvokeButton 1
triAssertNgsClient 14
triAssertMissionPlayable
// The client mirrors the server's war: objectives, its log, its squads.
triAssertIncludes [conquestState, "Morton=WEST"]
triAssertIncludes [conquestLog, "the war begins"]
triAssertExcludes [conquestState, "groups 0 "]
// Command from the client: hold the side, order squad 1.
triAssertEq [conquestCommanderView true, true]
triWait 2000
triAssertEq [conquestOrder [[1], 5283, 5375], 1]
conquestClientDone = 1
publicVariable "conquestClientDone"
// The server's answer comes back in the mirrored log.
triAssertIncludes [conquestLog, "commander: W1"]
triAssertEq [format["%1", conquestServerDone], "1"]
triEndTest
