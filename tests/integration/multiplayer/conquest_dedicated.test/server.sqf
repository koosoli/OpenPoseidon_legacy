// (triAssertNgs reads -1 from PoseidonServer on this build, in the stock probe too: the log asserts below wait instead.)
// The server runs the war: posts, squads, the player's slot adopted as a squad with HQ's orders.
triAssertIncludes [conquestState, "Morton=WEST"]
triAssertIncludes [conquestLog, "the war begins"]
triAssertIncludes [conquestLog, "player squad joins the war"]
triAssertIncludes [conquestLog, "HQ to the player squad"]
// The client's commander actions arrive and are carried out for its side.
triAssertEq [format["%1", conquestClientDone], "1"]
triAssertIncludes [conquestLog, "WEST is under a player's command"]
triAssertIncludes [conquestLog, "commander: W1"]
conquestServerDone = 1
publicVariable "conquestServerDone"
triWait 500
triEndTest
