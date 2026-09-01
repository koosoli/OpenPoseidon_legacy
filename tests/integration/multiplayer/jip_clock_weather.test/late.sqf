triAssertNgsClient 14
triAssertMissionPlayable

// THE ASSERT THIS TEST EXISTS FOR. This client connected after the server moved its
// clock five hours, so it never saw the change happen -- it can only know the time if
// the join handshake told it. Before that message existed this read the mission file's
// start time and was 0.2083 of a day out.
//
// The tolerance is 0.01 of a day (~14 minutes) and is for clock DRIFT between the
// server publishing its value and this line running, not for slop in the mechanism: a
// join that ignores the clock misses by twenty times this.
triAssertEq [format["%1", clockServerReady], "1"]
triAssertNear [dayTime, clockServerDayTime, 0.01]

// Weather has no SQF getter, so it cannot be asserted here. The server logs what it
// sent and the client logs what it applied ("JIP: clock/weather ..." on both sides);
// that pair is the evidence for the weather half.
clockLateDone = 1
publicVariable "clockLateDone"
