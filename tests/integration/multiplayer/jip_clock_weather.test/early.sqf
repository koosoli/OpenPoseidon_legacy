triAssertNgsClient 14
triAssertMissionPlayable
clockEarlyReady = 1
publicVariable "clockEarlyReady"

// The early client is the control. It was connected when the clock moved, so it follows
// by ordinary replication and must agree whether or not the join handshake carries the
// clock at all. If THIS assert fails the test is telling you about something else.
triAssertEq [format["%1", clockServerReady], "1"]
triAssertNear [dayTime, clockServerDayTime, 0.01]
clockEarlyDone = 1
publicVariable "clockEarlyDone"
